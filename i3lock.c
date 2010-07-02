/*
 * vim:ts=8:expandtab
 *
 * i3lock - an improved version of slock
 *
 * i3lock © 2009 Michael Stapelberg and contributors
 * i3lock © 2009 Jan-Erik Rediger
 * slock  © 2006-2008 Anselm R Garbe
 *
 * See file LICENSE for license information.
 *
 * Note that on any error (calloc is out of memory for example)
 * we do not do anything so that the user can fix the error by
 * himself (kill X to get more free memory or stop some other
 * program using SSH/console).
 *
 */
#define _XOPEN_SOURCE 500

#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <sys/types.h>
#include <X11/keysym.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/xpm.h>
#include <X11/extensions/dpms.h>
#include <stdbool.h>
#include <getopt.h>
#include <err.h>

#include <security/pam_appl.h>

#include "cursors.h"

static char passwd[256];

/*
 * displays an xpm image tiled over the whole screen
 * (the image will be visible on all screens
 * when using a multi monitor setup)
 *
 */
static void tile_image(XpmImage *image, int disp_height, int disp_width,
                       Display *dpy, Pixmap pix, Window w, GC gc)
{
        int rows = (int)ceil(disp_height / (float)image->height),
            cols = (int)ceil(disp_width / (float)image->width);

        for (int y = 0; y < rows; y++) {
                for (int x = 0; x < cols; x++) {
                        XCopyArea(dpy, pix, w, gc, 0, 0,
                                  image->width, image->height,
                                  image->width * x, image->height * y);
                }
        }
}

/*
 * Returns the colorpixel to use for the given hex color (think of HTML).
 *
 * The hex_color may not start with #, for example FF00FF works.
 *
 * NOTE that get_colorpixel() does _NOT_ check the given color code for validity.
 * This has to be done by the caller.
 *
 */
static uint32_t get_colorpixel(char *hex) {
        char strgroups[3][3] = {{hex[0], hex[1], '\0'},
                                {hex[2], hex[3], '\0'},
                                {hex[4], hex[5], '\0'}};
        uint32_t rgb16[3] = {(strtol(strgroups[0], NULL, 16)),
                             (strtol(strgroups[1], NULL, 16)),
                             (strtol(strgroups[2], NULL, 16))};

        return (rgb16[0] << 16) + (rgb16[1] << 8) + rgb16[2];
}

/*
 * Check if given file can be opened => exists
 *
 */
static bool file_exists(const char *filename)
{
        FILE * file = fopen(filename, "r");
        if(file)
        {
                fclose(file);
                return true;
        }
        return false;
}

/*
 * Puts the given XPM error code to stderr
 *
 */
static void print_xpm_error(int err)
{
        switch (err) {
                case XpmColorError:
                        fprintf(stderr, "XPM: Could not parse or alloc requested color\n");
                        break;
                case XpmOpenFailed:
                        fprintf(stderr, "XPM: Cannot open file\n");
                        break;
                case XpmFileInvalid:
                        fprintf(stderr, "XPM: invalid XPM file\n");
                        break;
                case XpmNoMemory:
                        fprintf(stderr, "XPM: Not enough memory\n");
                        break;
                case XpmColorFailed:
                        fprintf(stderr, "XPM: Color not found\n");
                        break;
        }
}


/*
 * Callback function for PAM. We only react on password request callbacks.
 *
 */
static int conv_callback(int num_msg, const struct pam_message **msg,
                         struct pam_response **resp, void *appdata_ptr)
{
        if (num_msg == 0)
                return 1;

        /* PAM expects an arry of responses, one for each message */
        if ((*resp = calloc(num_msg, sizeof(struct pam_message))) == NULL) {
                perror("calloc");
                return 1;
        }

        for (int c = 0; c < num_msg; c++) {
                if (msg[c]->msg_style != PAM_PROMPT_ECHO_OFF &&
                    msg[c]->msg_style != PAM_PROMPT_ECHO_ON)
                        continue;

                /* return code is currently not used but should be set to zero */
                resp[c]->resp_retcode = 0;
                if ((resp[c]->resp = strdup(passwd)) == NULL) {
                        perror("strdup");
                        return 1;
                }
        }

        return 0;
}

int main(int argc, char *argv[])
{
        char buf[32];
        char *username;
        int num, screen;

        unsigned int len;
        bool running = true;

        /* By default, fork, don’t beep and don’t turn off monitor */
        bool dont_fork = false;
        bool beep = false;
        bool dpms = false;
        bool xpm_image = false;
        bool tiling = false;
        char xpm_image_path[256];
        char color[7] = "ffffff"; // white

        unsigned char *curs = NULL;
        unsigned char *mask = NULL;
        unsigned int curs_w, curs_h;

        Cursor cursor;
        Display *dpy;
        KeySym ksym;
        Pixmap px_curs;
        Pixmap px_mask;
        Window root, w;
        XColor black, white, dummy;
        XEvent ev;
        XSetWindowAttributes wa;

        pam_handle_t *handle;
        struct pam_conv conv = {conv_callback, NULL};

        char opt;
        int optind = 0;
        static struct option long_options[] = {
                {"version", no_argument, NULL, 'v'},
                {"nofork", no_argument, NULL, 'n'},
                {"beep", no_argument, NULL, 'b'},
                {"dpms", no_argument, NULL, 'd'},
                {"image", required_argument, NULL, 'i'},
                {"color", required_argument, NULL, 'c'},
                {"tiling", no_argument, NULL, 't'},
                {"pointer", required_argument, NULL , 'p'},
                {NULL, no_argument, NULL, 0}
        };
        curs = curs_invisible_bits;
        mask = curs_invisible_bits;
        curs_w = curs_invisible_width;
        curs_h = curs_invisible_height;

        while ((opt = getopt_long(argc, argv, "vnbdi:c:tp:", long_options, &optind)) != -1) {
                switch (opt) {
                        case 'v':
                                errx(0, "i3lock-"VERSION", © 2009 Michael Stapelberg\n"
                                    "based on slock, which is © 2006-2008 Anselm R Garbe\n");
                        case 'n':
                                dont_fork = true;
                                break;
                        case 'b':
                                beep = true;
                                break;
                        case 'd':
                                dpms = true;
                                break;
                        case 'i':
                                strncpy(xpm_image_path, optarg, 255);
                                xpm_image = true;
                                break;
                        case 'c':
                        {
                                char *arg = optarg;
                                /* Skip # if present */
                                if (arg[0] == '#')
                                        arg++;

                                if (strlen(arg) != 6 || sscanf(arg, "%06[0-9a-fA-F]", color) != 1)
                                        errx(1, "color is invalid, color must be given in 6-byte format: rrggbb\n");

                                break;
                        }
                        case 't':
                                tiling = true;
                                break;
                        case 'p':
                                if (!strcmp(optarg, "default")) {
                                        curs = NULL;
                                        break;
                                }
                                if (!strcmp(optarg, "win")) {
                                        curs = curs_windows_bits;
                                        mask = mask_windows_bits;
                                        curs_w = curs_windows_width;
                                        curs_h = curs_windows_height;
                                        break;
                                }
                                break;
                        default:
                                errx(1, "i3lock: Unknown option. Syntax: i3lock [-v] [-n] [-b] [-d] [-i image.xpm] [-c color] [-t] [-p win|default]\n");
                }
        }

        if ((username = getenv("USER")) == NULL)
                errx(1, "USER environment variable not set, please set it.\n");

        int ret = pam_start("i3lock", username, &conv, &handle);
        if (ret != PAM_SUCCESS)
                errx(1, "PAM: %s\n", pam_strerror(handle, ret));

        if(!(dpy = XOpenDisplay(0)))
                errx(1, "i3lock: cannot open display\n");
        screen = DefaultScreen(dpy);
        root = RootWindow(dpy, screen);

        if (!dont_fork) {
                if (fork() != 0)
                        return 0;
        }

        /* init */
        wa.override_redirect = 1;
        wa.background_pixel = get_colorpixel(color);
        w = XCreateWindow(dpy, root, 0, 0, DisplayWidth(dpy, screen), DisplayHeight(dpy, screen),
                        0, DefaultDepth(dpy, screen), CopyFromParent,
                        DefaultVisual(dpy, screen), CWOverrideRedirect | CWBackPixel, &wa);
        XAllocNamedColor(dpy, DefaultColormap(dpy, screen), "black", &black, &dummy);
        XAllocNamedColor(dpy, DefaultColormap(dpy, screen), "white", &white, &dummy);
        if (curs != NULL) {
                px_curs = XCreateBitmapFromData(dpy, w, (char*) curs, curs_w, curs_h);
                px_mask = XCreateBitmapFromData(dpy, w, (char*) mask, curs_w, curs_h);
                cursor = XCreatePixmapCursor(dpy, px_curs, px_mask, &white, &black, 0, 0);
                XDefineCursor(dpy, w, cursor);
        } else {
                px_curs = 0;
                px_mask = 0;
                cursor = 0;
        }
        XMapRaised(dpy, w);

        if (xpm_image && file_exists(xpm_image_path)) {
                GC gc = XDefaultGC(dpy, 0);
                int depth = DefaultDepth(dpy, screen);
                int disp_width = DisplayWidth(dpy, screen);
                int disp_height = DisplayHeight(dpy, screen);
                Pixmap pix = XCreatePixmap(dpy, w, disp_width, disp_height, depth);
                XpmImage xpm_image;
                XpmInfo xpm_info;

                int err = XpmReadFileToXpmImage(xpm_image_path, &xpm_image, &xpm_info);
                if (err != 0) {
                        print_xpm_error(err);
                        return 1;
                }

                err = XpmCreatePixmapFromXpmImage(dpy, w, &xpm_image, &pix, 0, 0);
                if (err != 0) {
                        print_xpm_error(err);
                        return 1;
                }

                if (tiling)
                        tile_image(&xpm_image, disp_height, disp_width, dpy, pix, w, gc);
                else
                        XCopyArea(dpy, pix, w, gc, 0, 0, disp_width, disp_height, 0, 0);
        }

        for(len = 1000; len; len--) {
                if(XGrabPointer(dpy, root, False, ButtonPressMask | ButtonReleaseMask,
                        GrabModeAsync, GrabModeAsync, None, cursor, CurrentTime) == GrabSuccess)
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
        XSync(dpy, False);

        /* main event loop */
        while(running && !XNextEvent(dpy, &ev)) {
                if (len == 0 && dpms && DPMSCapable(dpy)) {
                        DPMSEnable(dpy);
                        DPMSForceLevel(dpy, DPMSModeOff);
                }

                if(ev.type != KeyPress)
                        continue;

                buf[0] = 0;
                num = XLookupString(&ev.xkey, buf, sizeof buf, &ksym, 0);
                if(IsKeypadKey(ksym)) {
                        if(ksym == XK_KP_Enter)
                                ksym = XK_Return;
                        else if(ksym >= XK_KP_0 && ksym <= XK_KP_9)
                                ksym = (ksym - XK_KP_0) + XK_0;
                }
                if(IsFunctionKey(ksym) ||
                   IsKeypadKey(ksym) ||
                   IsMiscFunctionKey(ksym) ||
                   IsPFKey(ksym) ||
                   IsPrivateKeypadKey(ksym))
                        continue;
                switch(ksym) {
                case XK_Return:
                        passwd[len] = 0;
                        /* Skip empty passwords */
                        if (len == 0)
                                continue;
                        if ((ret = pam_authenticate(handle, 0)) == PAM_SUCCESS)
                                running = false;
                        else {
                                fprintf(stderr, "PAM: %s\n", pam_strerror(handle, ret));
                                if (beep)
                                        XBell(dpy, 100);
                        }
                        len = 0;
                        break;
                case XK_Escape:
                        len = 0;
                        break;
                case XK_BackSpace:
                        if (len > 0)
                                len--;
                        break;
                default:
                        if(num && !iscntrl((int) buf[0]) && (len + num < sizeof passwd)) {
                                memcpy(passwd + len, buf, num);
                                len += num;
                        }
                        break;
                }
        }

        XUngrabPointer(dpy, CurrentTime);
        if (px_curs != 0) {
                XFreePixmap(dpy, px_curs);
                XFreePixmap(dpy, px_mask);
        }
        XDestroyWindow(dpy, w);
        XCloseDisplay(dpy);
        return 0;
}
