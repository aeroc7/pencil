#include <QPixmap>

#include "layervector.h"
#include "colormanager.h"
#include "strokemanager.h"
#include "layermanager.h"
#include "pencilsettings.h"
#include "editor.h"
#include "scribblearea.h"
#include "blitrect.h"

#include "pentool.h"


PenTool::PenTool( QObject *parent ) : StrokeTool( parent )
{
}

void PenTool::loadSettings()
{
    m_enabledProperties[WIDTH] = true;
    m_enabledProperties[PRESSURE] = true;
    m_enabledProperties[VECTORMERGE] = true;
    m_enabledProperties[ANTI_ALIASING] = true;

    QSettings settings( PENCIL2D, PENCIL2D );

    properties.width = settings.value( "penWidth" ).toDouble();
    properties.pressure = settings.value( "penPressure" ).toBool();
    properties.invisibility = OFF;
    properties.preserveAlpha = OFF;
    properties.useAA = settings.value( "brushAA").toBool();

    // First run
    if ( properties.width <= 0 )
    {
        setWidth(1.5);
        setPressure(1);
    }

    mCurrentWidth = properties.width;
}

void PenTool::setWidth(const qreal width)
{
    // Set current property
    properties.width = width;

    // Update settings
    QSettings settings( PENCIL2D, PENCIL2D );
    settings.setValue("penWidth", width);
    settings.sync();
}

void PenTool::setPressure( const bool pressure )
{
    // Set current property
    properties.pressure = pressure;

    // Update settings
    QSettings settings( PENCIL2D, PENCIL2D );
    settings.setValue("penPressure", pressure);
    settings.sync();
}

void PenTool::setAA( const bool AA )
{
    // Set current property
    properties.useAA = AA;

    // Update settings
    QSettings settings( PENCIL2D, PENCIL2D );
    settings.setValue("brushAA", AA);
    settings.sync();
}

QCursor PenTool::cursor()
{
    if ( isAdjusting ) // being dynamically resized
    {
        return circleCursors(); // two circles cursor
    }
    if ( mEditor->preference()->isOn( SETTING::DOTTED_CURSOR ) )
    {
        return dottedCursor(); // preview stroke size cursor
    }
    if ( mEditor->preference()->isOn( SETTING::TOOL_CURSOR ) )
    {
        return QCursor( QPixmap( ":icons/pen.png" ), 2, 22 );
    }
    return Qt::CrossCursor;
}

void PenTool::adjustPressureSensitiveProperties( qreal pressure, bool mouseDevice )
{
    mCurrentWidth = properties.width;

    if ( properties.pressure && !mouseDevice )
    {
        mCurrentPressure = pressure;
    }
    else
    {
        mCurrentPressure = 1.0;
    }
}

void PenTool::mousePressEvent( QMouseEvent *event )
{
    if ( event->button() == Qt::LeftButton )
    {
        mEditor->backup( typeName() );
        mScribbleArea->setAllDirty();
    }

    mMouseDownPoint = getCurrentPoint();
    mLastBrushPoint = getCurrentPoint();

    startStroke();
}

void PenTool::mouseReleaseEvent( QMouseEvent *event )
{
    Layer* layer = mEditor->layers()->currentLayer();

    if ( event->button() == Qt::LeftButton )
    {
        if ( isLayerPaintable( layer ) )
        {
            if (getCurrentPoint()==mMouseDownPoint)
            {
                paintAt(mMouseDownPoint);
            }
            else
            {
                drawStroke();
            }
        }

        if ( layer->type() == Layer::BITMAP )
        {
            mScribbleArea->paintBitmapBuffer();
            mScribbleArea->setAllDirty();
            mScribbleArea->clearBitmapBuffer();
        }
        else if ( layer->type() == Layer::VECTOR && mStrokePoints.size() > -1 )
        {
            // Clear the temporary pixel path
            mScribbleArea->clearBitmapBuffer();
            qreal tol = mScribbleArea->getCurveSmoothing() / mEditor->view()->scaling();
            BezierCurve curve( mStrokePoints, mStrokePressures, tol );
            curve.setWidth( properties.width );
            curve.setInvisibility( false );
            curve.setVariableWidth( properties.pressure );
            curve.setColourNumber( mEditor->color()->frontColorNumber() );

            auto pLayerVector = static_cast< LayerVector* >( layer );
            VectorImage* vectorImage = pLayerVector->getLastVectorImageAtFrame( mEditor->currentFrame(), 0 );
            vectorImage->addCurve( curve, mEditor->view()->scaling(), properties.vectorMergeEnabled );

            mScribbleArea->setModified( mEditor->layers()->currentLayerIndex(), mEditor->currentFrame() );
            mScribbleArea->setAllDirty();
        }
    }

    endStroke();
}

void PenTool::mouseMoveEvent( QMouseEvent *event )
{
    Layer* layer = mEditor->layers()->currentLayer();
    if ( layer->type() == Layer::BITMAP || layer->type() == Layer::VECTOR )
    {
        if ( event->buttons() & Qt::LeftButton )
        {
            drawStroke();
			//qDebug() << "DrawStroke" << event->pos() ;
        }
    }
}

// draw a single paint dab at the given location
void PenTool::paintAt( QPointF point )
{
    qDebug() << "Made a single dab at " << point;
    Layer* layer = mEditor->layers()->currentLayer();
    if ( layer->type() == Layer::BITMAP )
    {
        qreal opacity = 1.0f;
        if (properties.pressure == true)
        {
            opacity = mCurrentPressure / 2;
        }
        mCurrentWidth = properties.width;
        qreal brushWidth = mCurrentWidth;

        BlitRect rect;

        rect.extend( point.toPoint() );
        mScribbleArea->drawPen( point,
                                brushWidth,
                                mEditor->color()->frontColor(),
                                opacity,
                                properties.useAA);

        int rad = qRound( brushWidth ) / 2 + 2;
        mScribbleArea->refreshBitmap( rect, rad );
    }
}

void PenTool::drawStroke()
{
    StrokeTool::drawStroke();
    QList<QPointF> p = m_pStrokeManager->interpolateStroke();

    Layer* layer = mEditor->layers()->currentLayer();

    if ( layer->type() == Layer::BITMAP )
    {
        for ( int i = 0; i < p.size(); i++ )
        {
            p[ i ] = mEditor->view()->mapScreenToCanvas( p[ i ] );
        }

        qreal opacity = 1.0;
        qreal brushWidth = mCurrentPressure * mCurrentWidth;

        // TODO: Make popup widget for less important properties,
        // Eg. stepsize should be a slider.. will have fixed (0.3) value for now.
        qreal brushStep = (0.5 * brushWidth) - (0.3 * brushWidth * 0.5);
        brushStep = qMax( 1.0, brushStep );

        BlitRect rect;

        QPointF a = mLastBrushPoint;
        QPointF b = getCurrentPoint();

        qreal distance = 4 * QLineF( b, a ).length();
        int steps = qRound( distance ) / brushStep;

        for ( int i = 0; i < steps; i++ )
        {
            QPointF point = mLastBrushPoint + ( i + 1 ) * ( brushStep )* ( b - mLastBrushPoint ) / distance;
            rect.extend( point.toPoint() );
            mScribbleArea->drawPen( point,
                                    brushWidth,
                                    mEditor->color()->frontColor(),
                                    opacity,
                                    properties.useAA );

            if ( i == ( steps - 1 ) )
            {
                mLastBrushPoint = point;
            }
        }

        int rad = qRound( brushWidth ) / 2 + 2;
        mScribbleArea->refreshBitmap( rect, rad );
    }
    else if ( layer->type() == Layer::VECTOR )
    {
        qreal brushWidth = mCurrentPressure * mCurrentWidth;
        int rad = qRound( ( brushWidth / 2 + 2 ) * mEditor->view()->scaling() );

        QPen pen( mEditor->color()->frontColor(),
                  brushWidth * mEditor->view()->scaling(),
                  Qt::SolidLine,
                  Qt::RoundCap,
                  Qt::RoundJoin );

        if ( p.size() == 4 )
        {
            QPainterPath path( p[ 0 ] );
            path.cubicTo( p[ 1 ],
                          p[ 2 ],
                          p[ 3 ] );
            mScribbleArea->drawPath( path, pen, Qt::NoBrush, QPainter::CompositionMode_Source );
            mScribbleArea->refreshVector( path.boundingRect().toRect(), rad );
        }
    }
}
