/* See LICENSE file for license details. */
#define _XOPEN_SOURCE 500
#if HAVE_SHADOW_H
#include <shadow.h>
#endif

#include <ctype.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <linux/vt.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <fontconfig/fontconfig.h>
#include <time.h>
#include <X11/keysym.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/dpms.h>
#include <X11/Xft/Xft.h>

#if HAVE_BSD_AUTH
#include <login_cap.h>
#include <bsd_auth.h>
#endif

static void
die(const char *errstr, ...) {
    va_list ap;

    va_start(ap, errstr);
    vfprintf(stderr, errstr, ap);
    va_end(ap);
    exit(EXIT_FAILURE);
}

#ifndef HAVE_BSD_AUTH
static const char *
get_password() { /* only run as root */
    const char *rval;
    struct passwd *pw;

    if(geteuid() != 0)
        die("sflock: cannot retrieve password entry (make sure to suid sflock)\n");
    pw = getpwuid(getuid());
    endpwent();
    rval =  pw->pw_passwd;

#if HAVE_SHADOW_H
    {
        struct spwd *sp;
        sp = getspnam(getenv("USER"));
        endspent();
        rval = sp->sp_pwdp;
    }
#endif

    /* drop privileges temporarily */
    if (setreuid(0, pw->pw_uid) == -1)
        die("sflock: cannot drop privileges\n");
    return rval;
}
#endif

int
main(int argc, char **argv) {
    char curs[] = {0, 0, 0, 0, 0, 0, 0, 0};
    char buf[32], passwd[256], passdisp[256];
    int num, screen, width, height, update, sleepmode, term, pid;

#ifndef HAVE_BSD_AUTH
    const char *pws;
#endif
    unsigned int len;
    time_t wrong_time;
    Bool running = True;
    Cursor invisible;
    Display *dpy;
    KeySym ksym;
    Pixmap pmap;
    Window root, w;
    XColor normal_bg, error_bg, dummy;
    XEvent ev;
    XSetWindowAttributes wa;
    XftFont *font;
    XftColor xftcolor;
    XftDraw *xftdraw;
    XGlyphInfo extents;
    GC gc;
    XGCValues values;

    // defaults
    char* passchar = "*";
    char* fontname = "monospace";
    char* username = "";
    int showline = 1;
    int showusername = 1;
    int daemon = 0;
    int randchars = 0;
    unsigned int wrong_timeout = 0;
    char* normal_bg_color = "black";
    char* error_bg_color = "orange red";
    char* fg_color = "white";

    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-c")) {
            if (i + 1 < argc)
                passchar = argv[i + 1];
            else
                die("error: no password character given.\n");

        } else if (!strcmp(argv[i], "-f")) {
            if (i + 1 < argc)
                fontname = argv[i + 1];
            else
                die("error: font not specified.\n");

        } else if (!strcmp(argv[i], "-v"))
            die("sflock-"VERSION", © 2015 Ben Ruijl\n");

        else if (!strcmp(argv[i], "-h"))
            showline = 0;

        else if (!strcmp(argv[i], "-u"))
            showusername = 0;

        else if (!strcmp(argv[i], "-d"))
            daemon = 1;

        else if (!strcmp(argv[i], "-randchars")) {
            if (i+1 == argc)
                die("error: missing randchars value\n");
            randchars = atoi(argv[i + 1]);
            srand(time(NULL));

        } else if (!strcmp(argv[i], "-timeout")) {
            if (i+1 == argc)
                die("error: missing timeout value\n");
            wrong_timeout = atoi(argv[i + 1]);
            wrong_timeout = wrong_timeout>0 ? wrong_timeout : 0;

        } else if (!strcmp(argv[i], "-fg")) {
            if (i+1 == argc)
                die("error: missing fg value\n");
            fg_color = argv[i + 1];

        } else if (!strcmp(argv[i], "-bg")) {
            if (i+1 == argc)
                die("error: missing bg value\n");
            normal_bg_color = argv[i + 1];

        } else if (!strcmp(argv[i], "-errorbg")) {
            if (i+1 == argc)
                die("error: missing error bg value\n");
            error_bg_color = argv[i + 1];

        } else if (!strcmp(argv[i], "?"))
            die("usage: sflock\n"
                "           [-v] [-d] [-h] [-u]\n"
                "           [-c passchars]\n"
                "           [-f fontname]\n"
                "           [-fg fg]\n"
                "           [-bg bg]\n"
                "           [-errorbg errorbg]\n"
                "           [-timeout wrong password timeout]\n"
                "           [-randchars no. of random chars to display]\n"
            );
    }

    // fill with password characters
    for (int i = 0, passchar_len = strlen(passchar); i < sizeof passdisp; i++)
        passdisp[i] = passchar[i % passchar_len];


    /* disable tty switching */
    if ((term = open("/dev/console", O_RDWR)) == -1) {
        perror("error opening console");
    }

    if ((ioctl(term, VT_LOCKSWITCH)) == -1) {
        perror("error locking console");
    }

    /* deamonize */
    if (daemon) {
        pid = fork();
        if (pid < 0)
            die("Could not fork sflock.");
        if (pid > 0)
            exit(0); // exit parent
    }

#ifndef HAVE_BSD_AUTH
    pws = get_password();
    username = getpwuid(geteuid())->pw_name;
#else
    username = getlogin();
#endif

    if(!(dpy = XOpenDisplay(0)))
        die("sflock: cannot open dpy\n");

    screen = DefaultScreen(dpy);
    root = RootWindow(dpy, screen);
    width = DisplayWidth(dpy, screen);
    height = DisplayHeight(dpy, screen);

    XAllocNamedColor(dpy, DefaultColormap(dpy, screen), error_bg_color, &error_bg, &dummy);
    XAllocNamedColor(dpy, DefaultColormap(dpy, screen), normal_bg_color, &normal_bg, &dummy);

    wa.override_redirect = 1;
    wa.background_pixel = normal_bg.pixel;
    w = XCreateWindow(dpy, root, 0, 0, width, height,
            0, DefaultDepth(dpy, screen), CopyFromParent,
            DefaultVisual(dpy, screen), CWOverrideRedirect | CWBackPixel, &wa);

    pmap = XCreateBitmapFromData(dpy, w, curs, 8, 8);
    invisible = XCreatePixmapCursor(dpy, pmap, pmap, &normal_bg, &normal_bg, 0, 0);
    XDefineCursor(dpy, w, invisible);
    XMapRaised(dpy, w);

    xftdraw = XftDrawCreate(dpy, w, DefaultVisual(dpy, screen), DefaultColormap(dpy, screen));
    font = XftFontOpenName(dpy, screen, fontname);
    XftColorAllocName(dpy, DefaultVisual(dpy, screen), DefaultColormap(dpy, screen), fg_color, &xftcolor);

    if (font == 0) {
        die("error: could not find font. Try using a full description.\n");
    }

    gc = XCreateGC(dpy, w, (unsigned long)0, &values);
    XSetForeground(dpy, gc, xftcolor.pixel);

    for(len = 1000; len; len--) {
        if(XGrabPointer(dpy, root, False, ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                    GrabModeAsync, GrabModeAsync, None, invisible, CurrentTime) == GrabSuccess)
            break;
        usleep(1000);
    }
    if((running = running && (len > 0))) {
        for(len = 1000; len; len--) {
            if(XGrabKeyboard(dpy, root, True, GrabModeAsync, GrabModeAsync, CurrentTime)
                    == GrabSuccess)
                break;
            usleep(1000);
        }
        running = (len > 0);
    }

    len = 0;
    wrong_time = 0;
    XSync(dpy, False);
    update = True;
    sleepmode = False;

    /* main event loop */
    while(running && !XNextEvent(dpy, &ev)) {
        if (sleepmode) {
            DPMSEnable(dpy);
            DPMSForceLevel(dpy, DPMSModeOff);
            XFlush(dpy);
        }

        if (update) {
            unsigned int disp_len = (randchars && len) ? randchars : len;
            char* passstr = passdisp + ((randchars && len) ? (rand() % (sizeof(passdisp) - disp_len)) : 0);

#define draw_text_centered(text, len, y) \
            do { \
                XftTextExtentsUtf8(dpy, font, (XftChar8 *)text, len, &extents); \
                XftDrawStringUtf8(xftdraw, &xftcolor, font, (width - extents.width) / 2, y, (XftChar8 *)text, len); \
            } while(0)

            XClearWindow(dpy, w);
            if (showusername)
                draw_text_centered(username, strlen(username), height/2 - 10);

            if (showline)
                XDrawLine(dpy, w, gc, width * 3 / 8 , height / 2, width * 5 / 8, height / 2);
            draw_text_centered(passstr, disp_len, height/2 + font->height);
            update = False;
        }

        if (ev.type == MotionNotify) {
            sleepmode = False;
        }

        if(ev.type == KeyPress) {
            sleepmode = False;

            buf[0] = 0;
            num = XLookupString(&ev.xkey, buf, sizeof buf, &ksym, 0);
            if(IsKeypadKey(ksym)) {
                if(ksym == XK_KP_Enter)
                    ksym = XK_Return;
                else if(ksym >= XK_KP_0 && ksym <= XK_KP_9)
                    ksym = (ksym - XK_KP_0) + XK_0;
            }
            if(IsFunctionKey(ksym) || IsKeypadKey(ksym)
                    || IsMiscFunctionKey(ksym) || IsPFKey(ksym)
                    || IsPrivateKeypadKey(ksym))
                continue;

            if (wrong_time + wrong_timeout > time(NULL))
                continue;

            switch(ksym) {
                case XK_Return:
                    passwd[len] = 0;
#ifdef HAVE_BSD_AUTH
                    running = !auth_userokay(getlogin(), NULL, "auth-xlock", passwd);
#else
                    running = strcmp(crypt(passwd, pws), pws);
#endif
                    if (running != 0)
                        // change background on wrong password
                        XSetWindowBackground(dpy, w, error_bg.pixel);
                    len = 0;
                    time(&wrong_time);
                    break;
                case XK_Escape:
                    len = 0;

                    if (DPMSCapable(dpy)) {
                        sleepmode = True;
                    }

                    break;
                case XK_BackSpace:
                    if(len)
                        --len;
                    break;
                default:
                    if(num && !iscntrl((int) buf[0]) && (len + num < sizeof passwd)) {
                        memcpy(passwd + len, buf, num);
                        len += num;
                        XSetWindowBackground(dpy, w, normal_bg.pixel);
                    }

                    break;
            }

            update = True; // show changes
        }
    }

    /* free and unlock */
    setreuid(geteuid(), 0);
    if ((ioctl(term, VT_UNLOCKSWITCH)) == -1) {
        perror("error unlocking console");
    }

    close(term);
    setuid(getuid()); // drop rights permanently


    XUngrabPointer(dpy, CurrentTime);
    XFreePixmap(dpy, pmap);
    XftFontClose(dpy, font);
    XftColorFree(dpy, DefaultVisual(dpy, screen), DefaultColormap(dpy, screen), &xftcolor);
    XftDrawDestroy(xftdraw);
    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, w);
    XCloseDisplay(dpy);
    return 0;
}
