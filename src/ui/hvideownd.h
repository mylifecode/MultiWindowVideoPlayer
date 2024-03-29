#ifndef HVIDEOWND_H
#define HVIDEOWND_H

#include "qtheaders.h"
#include "hglwidget.h"
#include "hframe.h"

class HVideoWnd : public HGLWidget
{
    Q_OBJECT
public:
    explicit HVideoWnd(QWidget *parent = nullptr);

signals:

protected:
    void calFps();
    virtual void paintGL();
    virtual void drawFPS();

public slots:

public:
    HFrame last_frame;

    // for calFPS
    uint64_t tick;
    int framecnt;
    int fps;
};

#endif // HVIDEOWND_H
