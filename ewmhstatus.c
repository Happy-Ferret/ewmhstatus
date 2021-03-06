#include <fcntl.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <signal.h>
#include <ctype.h>
#include <xcb/xcb.h>
#include <xcb/xcb_event.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xcb_ewmh.h>
#include <cairo/cairo.h>
#include "ewmhstatus.h"

#define MAX(A, B)         ((A) > (B) ? (A) : (B))
#define MIN(A, B)         ((A) > (B) ? (B) : (A))
#define ABS(A, B)         ((A) > (B) ? (A - B) : (B - A))

#define FIFO_ENV_VAR   "EWMHSTATUS_FIFO"
#define MISSING_VALUE  " "
#define FONT_FAMILY    "sans-serif"
#define FONT_SIZE      11
#define HORIZ_PADDING  9

xcb_connection_t *dpy;
xcb_ewmh_connection_t *ewmh;
xcb_screen_t *screen;
int default_screen;
xcb_window_t cur_win;

uint16_t screen_width;
unsigned int horiz_padding = HORIZ_PADDING;
unsigned int cur_desktop, num_desktops;

char desktop_name[BUFSIZ] = MISSING_VALUE;
char window_title[BUFSIZ] = MISSING_VALUE;
char external_infos[BUFSIZ] = MISSING_VALUE;

char font_family[BUFSIZ] = FONT_FAMILY;
int font_size = FONT_SIZE;

char *fifo_path;
int fifo_fd, dpy_fd, sel_fd;

bool running;

double text_width(char *s)
{
    int w;
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 1, 1);
    cairo_t *cr = cairo_create(surface);
    cairo_text_extents_t te;
    cairo_select_font_face(cr, font_family, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, font_size);
    cairo_text_extents(cr, s, &te);
    w = te.x_advance;
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    return w;
}

void truncate_text(char *s, double width, unsigned int lower, unsigned int upper)
{
    if (ABS(lower, upper) < 2) {
        s[lower] = '\0';
    } else {
        unsigned int middle = (lower + upper) / 2;
        char c = s[middle];
        s[middle] = '\0';
        double w = text_width(s);
        s[middle] = c;
        if (w < width)
            truncate_text(s, width, middle, upper);
        else
            truncate_text(s, width, lower, middle);
    }
}

void copy_prop(char *dest, char *src, int len, int idx, int num_itm)
{
    if (num_itm <= 1) {
        strncpy(dest, src, MIN(len, BUFSIZ));
        dest[len] = '\0';
    } else {
        int pos = 0, cnt = 0;
        while (cnt < idx && cnt < (num_itm - 1) && pos < len) {
            pos += strlen(src + pos) + 1;
            cnt++;
        }
        if (cnt == (num_itm - 1))
            copy_prop(dest, src + pos, len - pos, 0, 1);
        else
            strncpy(dest, src + pos, BUFSIZ);
    }
}

void handle_signal(int sig)
{
    if (sig == SIGTERM || sig == SIGINT || sig == SIGHUP)
        running = false;
}

void update_cur_desktop(void)
{
    xcb_ewmh_get_current_desktop_reply(ewmh, xcb_ewmh_get_current_desktop(ewmh, default_screen), &cur_desktop, NULL);
}

void update_num_desktops(void)
{
    xcb_ewmh_get_number_of_desktops_reply(ewmh, xcb_ewmh_get_number_of_desktops(ewmh, default_screen), &num_desktops, NULL);
}

void update_window_title(void)
{
    xcb_window_t win;
    xcb_ewmh_get_utf8_strings_reply_t ewmh_txt_prop;
    xcb_icccm_get_text_property_reply_t icccm_txt_prop;
    uint32_t values[] = {XCB_EVENT_MASK_PROPERTY_CHANGE};
    uint32_t values_reset[] = {XCB_EVENT_MASK_NO_EVENT};

    ewmh_txt_prop.strings = NULL;
    icccm_txt_prop.name = NULL;

    if (xcb_ewmh_get_active_window_reply(ewmh, xcb_ewmh_get_active_window(ewmh, default_screen), &win, NULL) == 1
            && (xcb_ewmh_get_wm_name_reply(ewmh, xcb_ewmh_get_wm_name(ewmh, win), &ewmh_txt_prop, NULL) == 1
                || xcb_icccm_get_wm_name_reply(dpy, xcb_icccm_get_wm_name(dpy, win), &icccm_txt_prop, NULL) == 1)) {
        if (ewmh_txt_prop.strings != NULL && ewmh_txt_prop.strings_len > 0) {
            copy_prop(window_title, ewmh_txt_prop.strings, ewmh_txt_prop.strings_len, 0, 1);
        } else if (icccm_txt_prop.name != NULL && icccm_txt_prop.name_len > 0) {
            copy_prop(window_title, icccm_txt_prop.name, icccm_txt_prop.name_len, 0, 1);
        } else {
            strcpy(window_title, MISSING_VALUE);
        }
        if (win != cur_win) {
            xcb_change_window_attributes(dpy, cur_win, XCB_CW_EVENT_MASK, values_reset);
            cur_win = win;
        }
        xcb_generic_error_t *err = xcb_request_check(dpy, xcb_change_window_attributes_checked(dpy, win, XCB_CW_EVENT_MASK, values));
        if (err != NULL)
            fprintf(stderr, "could not capture property change events on window 0x%X\n", win);
    } else {
        strcpy(window_title, MISSING_VALUE);
    }
}

void update_desktop_name(void)
{
    xcb_ewmh_get_utf8_strings_reply_t names;

    if (xcb_ewmh_get_desktop_names_reply(ewmh, xcb_ewmh_get_desktop_names(ewmh, default_screen), &names, NULL) == 1)
        copy_prop(desktop_name, names.strings, names.strings_len, cur_desktop, num_desktops);
    else
        strcpy(desktop_name, MISSING_VALUE);
}

void output_infos(void)
{
    double left_width = text_width(desktop_name);
    double right_width = text_width(external_infos);
    double center_width = text_width(window_title);
    double available_center = screen_width - (left_width + right_width + 4 * horiz_padding);

    if (center_width > available_center) {
        truncate_text(window_title, available_center, 0, strlen(window_title) - 1);
        center_width = text_width(window_title);
    }

    int left_pos = horiz_padding;
    int right_pos = screen_width - horiz_padding - right_width;
    int center_pos = left_width + 2 * horiz_padding + (available_center - center_width) / 2;

    printf("^pa(%i)%s^pa(%i)%s^pa(%i)%s\n", center_pos, window_title, right_pos, external_infos, left_pos, desktop_name);
    fflush(stdout);
}

void handle_event(xcb_generic_event_t *evt)
{
    xcb_property_notify_event_t *pne;
    switch (XCB_EVENT_RESPONSE_TYPE(evt)) {
        case XCB_PROPERTY_NOTIFY:
            pne = (xcb_property_notify_event_t *) evt;
            if (pne->atom == ewmh->_NET_DESKTOP_NAMES) {
                update_desktop_name();
                output_infos();
            } else if (pne->atom == ewmh->_NET_ACTIVE_WINDOW) {
                update_window_title();
                output_infos();
            } else if (pne->window != screen->root && (pne->atom == ewmh->_NET_WM_NAME || pne->atom == XCB_ATOM_WM_NAME)) {
                update_window_title();
                output_infos();
            } else if (pne->atom == ewmh->_NET_NUMBER_OF_DESKTOPS) {
                update_num_desktops();
            } else if (pne->atom == ewmh->_NET_CURRENT_DESKTOP) {
                update_cur_desktop();
                update_desktop_name();
                output_infos();
            }
        default:
            break;
    }
}

void register_events(void)
{
    uint32_t values[] = {XCB_EVENT_MASK_PROPERTY_CHANGE};
    xcb_generic_error_t *err = xcb_request_check(dpy, xcb_change_window_attributes_checked(dpy, screen->root, XCB_CW_EVENT_MASK, values));
    if (err != NULL)
        running = false;
}

void setup(void)
{
    dpy = xcb_connect(NULL, &default_screen);
    ewmh = malloc(sizeof(xcb_ewmh_connection_t));
    xcb_intern_atom_cookie_t *ewmh_cookies = xcb_ewmh_init_atoms(dpy, ewmh);
    xcb_ewmh_init_atoms_replies(ewmh, ewmh_cookies, NULL);
    screen = ewmh->screens[default_screen];
    screen_width = screen->width_in_pixels;
    fifo_path = getenv(FIFO_ENV_VAR);
    /* http://www.outflux.net/blog/archives/2008/03/09/using-select-on-a-fifo/ */
    if (fifo_path != NULL)
        fifo_fd = open(fifo_path, O_RDWR | O_NONBLOCK);
    else
        fifo_fd = -1;
    dpy_fd = xcb_get_file_descriptor(dpy);
    sel_fd = MAX(fifo_fd, dpy_fd) + 1;
    cur_win = num_desktops = cur_desktop = 0;
    running = true;
}

int main(int argc, char *argv[])
{
    fd_set descriptors;
    xcb_generic_event_t *evt;
    signal(SIGTERM, handle_signal);
    signal(SIGINT, handle_signal);
    signal(SIGHUP, handle_signal);
    setup();
    register_events();
    update_num_desktops();
    update_cur_desktop();
    update_desktop_name();
    update_window_title();

    if (argc > 1) {
        strcpy(font_family, argv[1]);
        if (argc > 2) {
            font_size = atoi(argv[2]);
            if (argc > 3)
                horiz_padding = atoi(argv[3]);
        }
    }

    output_infos();
    xcb_flush(dpy);

    while (running) {
        FD_ZERO(&descriptors);
        FD_SET(fifo_fd, &descriptors);
        FD_SET(dpy_fd, &descriptors);

        if (select(sel_fd, &descriptors, NULL, NULL, NULL)) {
            if (FD_ISSET(dpy_fd, &descriptors)) {
                while ((evt = xcb_poll_for_event(dpy)) != NULL) {
                    handle_event(evt);
                    free(evt);
                }
            }

            if (FD_ISSET(fifo_fd, &descriptors)) {
                int bytes = read(fifo_fd, external_infos, sizeof(external_infos));
                if (bytes > 0) {
                    external_infos[bytes] = '\0';
                    int i = bytes - 1;
                    while (i > 0 && isspace(external_infos[i]))
                        external_infos[i--] = '\0';
                    output_infos();
                }
            }
        }
    }

    close(fifo_fd);
    xcb_ewmh_connection_wipe(ewmh);
    free(ewmh);
    xcb_disconnect(dpy);
    return 0;
}
