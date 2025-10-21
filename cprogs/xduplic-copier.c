/*
 * Copyright (C) 2002,2013 Ian Jackson <ian@chiark.greenend.org.uk>
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 3,
 * or (at your option) any later version.
 *
 * This is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/cursorfont.h>
#include <X11/cursorfont.h>
#include <X11/Xmu/WinUtil.h>
#include <X11/XKBlib.h>

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>

static Display *display;
static int selecting, l1_x, l1_y;
static char stringbuf[20];
static unsigned long c_black, c_white, c_red, c_yellow;
static Window w, root;
static Cursor cursor;
static GC gc;
static Colormap cmap;

struct wnode {
  struct wnode *next;
  Window w;
} *headwn;

static unsigned long getcolour(const char *name, int def) {
  Status st;
  XColor screen_def, exact_def;
  st= XAllocNamedColor(display,cmap,name,&screen_def,&exact_def);
  fprintf(stdout,"name %s pixel %lu\n",name,screen_def.pixel);
  return st ? screen_def.pixel : def;
}

static void beep(void) { XBell(display,100); }

/*
 *  While selecting:
 *                   Left                         Right
 *    application     select                       deselect
 *    root            start typing                 deselect all
 *    xduplic         start typing                 quit
 *
 *  While typing:
 *                   Left                         Right
 *    xduplic         start selecting              quit
 *
 *  Colours:
 *
 *    While typing:                   yellow on black
 *    While selecting:                white on black
 *    Idle (typing into nowhere):     red on black
 */

static void redisplay(void) {
  XClearWindow(display,w);
  XDrawString(display,w,gc, l1_x,l1_y, stringbuf,strlen(stringbuf));
}

static void restatus(void) {
  XGCValues v;
  int count;
  struct wnode *own;

  v.foreground= (selecting ? c_white :
		 headwn ? c_yellow :
		 c_red);
  
  XChangeGC(display,gc,
	    GCForeground,
	    &v);
  XClearWindow(display,w);

  for (count=0, own=headwn; own; own=own->next) count++;
  snprintf(stringbuf,sizeof(stringbuf),
	   "%c %d",
	   selecting ? 'S' :
	   headwn ? 'T' : 'i',
	   count);
  redisplay();
}

static void stopselecting(void) {
  XUngrabPointer(display,CurrentTime);
  selecting= 0;
  restatus();
}

static void startselecting(void) {
  Status st;
  st= XGrabPointer(display,root,True,
		   ButtonPressMask,GrabModeAsync,
		   GrabModeAsync,None,cursor,CurrentTime);
  if (st != Success) beep();
  else selecting= 1;
  restatus();
}

static void buttonpress(XButtonEvent *e) {
  struct wnode *own, **ownp, *ownn;
  int rightbutton;
  Window sw;

  switch (e->button) {
  case Button1: rightbutton=0; break;
  case Button3: rightbutton=1; break;
  default: return;
  }

  fprintf(stdout,"button right=%d in=%lx sub=%lx (w=%lx root=%lx)\n",
	  rightbutton, (unsigned long)e->window, (unsigned long)e->subwindow,
	  (unsigned long)w, (unsigned long)e->root);

  if (e->window == w) {
    if (rightbutton) _exit(0);
    if (selecting) {
      stopselecting();
      /* move pointer to where it already is, just in case wm is confused */
      XWarpPointer(display,None,root, 0,0,0,0, e->x_root,e->y_root);
    } else {
      startselecting();
    }
    return;
  }

  if (!selecting) return;

  if (e->window != e->root) return;

  if (!e->subwindow) {
    if (!rightbutton) {
      stopselecting();
    } else {
      if (!headwn) { beep(); return; }
      for (own=headwn; own; own=ownn) {
	ownn= own->next;
	free(own);
      }
      headwn= 0;
      restatus();
    }
    return;
  }

  sw= XmuClientWindow(display, e->subwindow);

  if (sw == w) { beep(); return; }

  for (ownp=&headwn;
       (own=(*ownp)) && own->w != sw;
       ownp= &(*ownp)->next);
  
  if (!rightbutton) {
    
    if (own) { beep(); return; }
    own= malloc(sizeof(*own)); if (!own) { perror("malloc"); exit(-1); }
    own->w= sw;
    own->next= headwn;
    headwn= own;

  } else {

    if (!own) { beep(); return; }
    *ownp= own->next;
    free(own);

  }

  restatus();
}

static void keypress(XKeyEvent *e) {
  Status st;
  struct wnode *own;
  unsigned long mask;
  
  if (selecting) {
    fprintf(stdout,"key type %d serial %lu (send %d) "
	    "window %lx root %lx sub %lx time %lx @%dx%d (%dx%dabs) "
	    "state %x keycode %u same %d\n",
	    e->type, e->serial, (int)e->send_event,
	    (unsigned long)e->window,
	    (unsigned long)e->root,
	    (unsigned long)e->subwindow,
	    (unsigned long)e->time,
	    e->x,e->y, e->x_root,e->y_root,
	    e->state, e->keycode, (int)e->same_screen);
    if (XkbKeycodeToKeysym(display, e->keycode, 0, 0) == XK_q) _exit(1);
    beep(); return;
  }
  for (own=headwn; own; own=own->next) {
    mask= (e->type == KeyPress ? KeyPressMask :
	   e->type == KeyRelease ? KeyReleaseMask :
	   KeyPressMask|KeyReleaseMask);
    e->window= own->w;
    e->subwindow= None;
    e->send_event= True;
    st= XSendEvent(display,own->w,True,mask,(XEvent*)e);
    if (st != Success) {
      fprintf(stdout,"sendevent to %lx %d mask %lx\n",
	      (unsigned long)own->w, st, mask);
    }
  }
}

static void expose(XExposeEvent *e) {
  if (e->count) return;
  redisplay();
}

int main(int argc, const char **argv) {
  XEvent e;
  XGCValues gcv;
  XSetWindowAttributes wv;
  int screen, direction, ascent, descent, l1_width, l1_height;
  XCharStruct overall;
  Font font;

  display= XOpenDisplay(0);
  if (!display) { fputs("XOpenDisplay failed\n",stderr); exit(-1); }
  screen= DefaultScreen(display);
  cmap= DefaultColormap(display,screen);
  root= DefaultRootWindow(display);
  
  c_black=   getcolour("black",  0);
  c_white=   getcolour("white",  1);
  c_yellow=  getcolour("yellow", c_white);
  c_red=     getcolour("red",    c_white);

  cursor= XCreateFontCursor(display,XC_crosshair);

  wv.event_mask= KeyPressMask|KeyReleaseMask|ButtonPressMask|ExposureMask;
  w= XCreateWindow(display, root,
		   0,0, 50,21, 0,DefaultDepth(display,screen),
		   InputOutput, DefaultVisual(display,screen),
		   CWEventMask, &wv);

  font= XLoadFont(display,"fixed");

  gcv.background= c_black;
  gcv.font=       font;
  gc= XCreateGC(display,w,GCBackground|GCFont,&gcv);

  XQueryTextExtents(display,font, "SIT 0689", 8,
		    &direction,&ascent,&descent,&overall);
  l1_width= overall.lbearing + overall.rbearing;
  l1_x= overall.lbearing;
  l1_y= ascent;
  l1_height= descent+ascent;

  XResizeWindow(display,w, l1_width,l1_height);
  XSetWindowBackground(display,w,c_black);
  
  XMapWindow(display,w);
  restatus();
  
  for (;;) {
    XNextEvent(display,&e);
    fprintf(stdout,"selecting = %d; event type = %lu\n",
	    selecting, (unsigned long)e.type);
    switch (e.type) {
    case Expose:                     expose(&e.xexpose);       break;
    case ButtonPress:                buttonpress(&e.xbutton);  break;
    case KeyPress: case KeyRelease:  keypress(&e.xkey);        break;
    }
  }
}
