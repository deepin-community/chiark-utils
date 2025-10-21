/*
 * The display is a lin-log chart of the charge over time.
 * Time is on a log scale.
 * The top pixel line is 60s from now.
 * The bottom of the window is 1h from now.
 *
 * display outputs, per line:
 *
 *   Remaining:	 | Empty:	| Degraded:
 *     blue	 |  black	|  dimgrey	discharging
 *     green	 |  black	|  dimgrey	charging
 *     cyan	 |  black	|  dimgrey	charged
 *     grey	 |  black	|  dimgrey	charging&discharging!
 *     lightgrey |  black	|  dimgrey	none of the above
 *     blue	 |  red  	|  dimgrey	discharging - low!
 *     green	 |  red  	|  dimgrey	charging - low
 *     cyan	 |  red  	|  dimgrey	charged - low [1]
 *     grey	 |  red  	|  dimgrey	charging&discharging, low [1]
 *       ...  darkgreen  ...			no batteries present
 *       ...  yellow  ...			error
 *
 * [1] battery must be quite badly degraded
 */
/*
 * Copyright (C) 2004 Ian Jackson <ian@davenant.greenend.org.uk>
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
 * You should have received a copy of the GNU General Public
 * License along with this file; if not, consult the Free Software
 * Foundation's website at www.fsf.org, or the GNU Project website at
 * www.gnu.org.
 */

#include <stdio.h>
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <ctype.h>
#include <stdint.h>
#include <limits.h>
#include <inttypes.h>

#include <sys/poll.h>
#include <sys/types.h>
#include <dirent.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xresource.h>

#define TOP      60
#define BOTTOM 3600

#define TIMEOUT         5000 /* milliseconds */
#define TIMEOUT_ONERROR 3333 /* milliseconds */

static const char program_name[]= "xbatmon-simple";
static int debug=-1, alarmlevel;

/*---------- general utility stuff and declarations ----------*/

static void fail(const char *m) {
  fprintf(stderr,"error: %s\n", m);
  exit(-1);
}
static void badusage(void) { fail("bad usage"); }

typedef uint64_t value;
#define VAL_NOTFOUND (~(value)0)

typedef struct fileinfo fileinfo;
typedef int parser(const fileinfo*);

static parser parse_uevent;

struct fileinfo {
  const char *filename;
  parser *parse;
  const void *extra;
};

/*---------- structure of and results from /sys/class/power/... ----------*/
/* variables this_... are the results from readbattery();
 * if readbattery() succeeds the appropriate ones are all valid
 * and not VAL_NOTFOUND
 */

typedef struct batinfo_field {
  const char *label;
  value *valuep;
  const char *enumarray[10];
} batinfo_field;

#define BAT_QTYS(_, _ec, EC_, PC_)				\
  _(design_capacity##_ec,    BATTERY,  EC_##FULL_DESIGN )	\
  _(last_full_capacity##_ec, BATTERY,  EC_##FULL        )	\
  _(remaining_capacity##_ec, BATTERY,  EC_##NOW         )	\
  _(present_rate##_ec,       BATTERY,  PC_##NOW         )
 /* ENERGY [mWh]; POWER [mW]; CHARGE [uAh]; CURRENT [uA] */

#define UEVENT_ESSENTIAL_QUANTITY_FIELDS(_)			\
  _(present,                 BATTERY,  PRESENT /* bool */ )	\
  _(online,                  MAINS,    ONLINE  /* bool */ )

#define UEVENT_FUNKY_QUANTITY_FIELDS(_)		\
  BAT_QTYS(_,_energy,ENERGY_,POWER_)		\
  BAT_QTYS(_,_charge,CHARGE_,CURRENT_)

#define UEVENT_OPTIONAL_QUANTITY_FIELDS(_)			\
  _(voltage,                 BATTERY,  VOLTAGE_NOW /* uV */ )

#define UEVENT_ENUM_FIELDS(_)						\
  _(state,   BATTERY,  STATUS,  "Discharging","Charging","Full","Unknown" ) \
  _(type,    BOTH,     TYPE,    "Mains",       "Battery"              )

#define CHGST_DISCHARGING 0 /* Reflects order in _(state,...) above     */
#define CHGST_CHARGING    1 /* Also, much code assumes exactly          */
#define CHGST_CHARGED     2 /* these three possible states.             */
#define CHGST_UNKNOWN     3 /* these three possible states.             */
#define CHGST_ERROR       8 /* Except that this one is an extra bit.    */

#define TYPE_MAINS        0 /* Reflects order in _(type,...) above        */
#define TYPE_BATTERY      1 /* Also, much code assumes exactly these two  */
#define TYPE_BOTH       100 /* Except this is a magic invalid value.      */

#define SEPARATE_QUANTITY_FIELDS(_)		\
  /* See commit ec6f5f0be800bc5f2a27046833dba04e0c67ffac for
     the code needed to use this */


#define ALL_DIRECT_VARS(_)			\
  UEVENT_ESSENTIAL_QUANTITY_FIELDS(_)		\
  UEVENT_FUNKY_QUANTITY_FIELDS(_)		\
  UEVENT_OPTIONAL_QUANTITY_FIELDS(_)		\
  UEVENT_ENUM_FIELDS(_)				\
  SEPARATE_QUANTITY_FIELDS(_)

#define ALL_VARS(_)				\
  ALL_DIRECT_VARS(_)				\
  BAT_QTYS(_,,,)

#define ALL_NEEDED_FIELDS(_)			\
  UEVENT_ESSENTIAL_QUANTITY_FIELDS(_)		\
  UEVENT_ENUM_FIELDS(_)				\
  SEPARATE_QUANTITY_FIELDS(_)

#define ALL_PLAIN_ACCUMULATE_FIELDS(_)		\
  UEVENT_ESSENTIAL_QUANTITY_FIELDS(_)		\
  SEPARATE_QUANTITY_FIELDS(_)

#define ALL_ACCUMULATE_FIELDS(_)		\
  ALL_PLAIN_ACCUMULATE_FIELDS(_)		\
  BAT_QTYS(_,,,)


#define F_VAR(f,...) \
static value this_##f;
ALL_VARS(F_VAR)

#define Q_FLD(f,t,l)        { "POWER_SUPPLY_" #l, &this_##f },
#define E_FLD(f,t,l,vl...)  { "POWER_SUPPLY_" #l, &this_##f, { vl } },

static const batinfo_field uevent_fields[]= {
  UEVENT_ESSENTIAL_QUANTITY_FIELDS(Q_FLD)
  UEVENT_FUNKY_QUANTITY_FIELDS(Q_FLD)
  UEVENT_OPTIONAL_QUANTITY_FIELDS(Q_FLD)
  UEVENT_ENUM_FIELDS(E_FLD)
  { 0 }
};

#define S_FLD(f,t,fn,vl...)						\
static const batinfo_field bif_##f = { 0, &this_##f, { vl } };
  SEPARATE_QUANTITY_FIELDS(S_FLD)

#define S_FILE(f,t,fn,vl...) { fn, parse_separate, &bif_##f },

static const fileinfo files[]= {
  { "uevent",  parse_uevent,  uevent_fields },
  SEPARATE_QUANTITY_FIELDS(S_FILE)
  { 0 }
};

/*---------- parsing of one thingx in /sys/class/power/... ----------*/

/* variables private to the parser and its error handlers */
static char batlinebuf[1000];
static FILE *batfile;
static const char *batdirname;
static const char *batfilename;
static const char *batlinevalue;

static int batfailf(const char *why) {
  if (batlinevalue) {
    fprintf(stderr,"%s/%s: %s value `%s': %s\n",
	    batdirname,batfilename, batlinebuf,batlinevalue,why);
  } else {
    fprintf(stderr,"%s/%s: %s: `%s'\n",
	    batdirname,batfilename, why, batlinebuf);
  }
  return -1;
}

static int batfailc(const char *why) {
  fprintf(stderr,"%s/%s: %s\n",
	  batdirname,batfilename, why);
  return -1;
}

static int batfaile(const char *syscall, const char *target) {
  fprintf(stderr,"%s: failed to %s %s: %s\n",
	  batdirname ? batdirname : "*", syscall, target, strerror(errno));
  return -1;
}

static int chdir_base(void) {
  int r;
  
  r= chdir("/sys/class/power_supply");
  if (r) return batfaile("chdir","/sys/class/power_supply");

  return 0;
}

static void tidybattery(void) {
  if (batfile) { fclose(batfile); batfile=0; }
}

static int parse_value(const fileinfo *cfile, const batinfo_field *field) {
  if (*field->valuep != VAL_NOTFOUND)
    return batfailf("value specified multiple times");

  if (!field->enumarray[0]) {

    char *ep;
    *field->valuep= strtoull(batlinevalue,&ep,10);
    if (*ep)
      batfailf("value number syntax incorrect");

  } else {
	
    const char *const *enumsearch;
    for (*field->valuep=0, enumsearch=field->enumarray;
	 *enumsearch && strcmp(*enumsearch,batlinevalue);
	 (*field->valuep)++, enumsearch++);
    if (!*enumsearch)
      batfailf("unknown enum value");

  }
  return 0;
}

static int parse_uevent(const fileinfo *cfile) {
  char *equals= strchr(batlinebuf,'=');
  if (!equals)
    return batfailf("line without a equals");
  *equals= 0;
  batlinevalue = equals+1;

  const batinfo_field *field;
  for (field=cfile->extra; field->label; field++) {
    if (!strcmp(field->label,batlinebuf))
      goto found;
  }
  return 0;

 found:
  return parse_value(cfile, field);
}

static int readbattery(void) { /* 0=>ok, -1=>couldn't */
  
  const fileinfo *cfile;
  char *sr;
  int r, l;
  
  r= chdir_base();
  if (r) return r;

  r= chdir(batdirname);
  if (r) return batfaile("chdir",batdirname);

#define V_NOTFOUND(f,...) \
  this_##f = VAL_NOTFOUND;
ALL_VARS(V_NOTFOUND)

  for (cfile=files;
       (batfilename= cfile->filename);
       cfile++) {
    batfile= fopen(batfilename,"r");
    if (!batfile) {
      if (errno == ENOENT) continue;
      return batfaile("open",batfilename);
    }

    for (;;) {
      batlinevalue= 0;
      
      sr= fgets(batlinebuf,sizeof(batlinebuf),batfile);
      if (ferror(batfile)) return batfaile("read",batfilename);
      if (!sr && feof(batfile)) break;
      l= strlen(batlinebuf);
      assert(l>0);
      if (batlinebuf[l-1] != '\n')
	return batfailf("line too long");
      batlinebuf[l-1]= 0;

      if (cfile->parse(cfile))
	return -1;
    }

    fclose(batfile);
    batfile= 0;
  }

  if (debug) {
    printf("%s:\n",batdirname);
#define V_PRINT(f,...)					\
    printf(" %-30s = %20"PRId64"\n", #f, (int64_t)this_##f);
ALL_DIRECT_VARS(V_PRINT)
  }

  if (this_type == -1) {
    /* some kernels don't seem to provide TYPE in the uevent
     * guess the type from whether we see "present" or "online" */
    if (this_online >= 0 && this_present == -1) this_type = TYPE_MAINS;
    if (this_online == -1 && this_present >= 0) this_type = TYPE_BATTERY;
    if (debug)
      printf(" type absent from uevent %6s guessed %12s %"PRId64"\n",
	     "", "", this_type);
  }

  int needsfields_MAINS   = this_type == TYPE_MAINS;
  int needsfields_BATTERY = this_type == TYPE_BATTERY;
  int needsfields_BOTH    = 1;

  int missing = 0;

#define V_NEEDED(f,t,...)				\
  if (needsfields_##t && this_##f == VAL_NOTFOUND) {	\
    fprintf(stderr,"%s: %s: not found\n",		\
	    batdirname, #f);				\
    missing++;						\
  }
ALL_NEEDED_FIELDS(V_NEEDED)

  if (missing) return -1;

  return 0;
}   

/*---------- data collection and analysis ----------*/

/* These next three variables are the results of the charging state */
static unsigned charging_mask; /* 1u<<CHGST_* | ... */
static double nondegraded_norm, fill_norm, ratepersec_norm;
static int alarmed;

#define Q_VAR(f,t,...) \
static double total_##f;
  ALL_ACCUMULATE_FIELDS(Q_VAR)

static void acquiredata(void) {
  DIR *di = 0;
  struct dirent *de;
  int r;
  
  charging_mask= 0;
  alarmed = 0;

  if (debug) printf("\n");

#define Q_ZERO(f,t,...) \
  total_##f= 0;
ALL_ACCUMULATE_FIELDS(Q_ZERO)

  r = chdir_base();
  if (r) goto bad;

  di= opendir(".");  if (!di) { batfaile("opendir","battery"); goto bad; }
  while ((de= readdir(di))) {
    if (de->d_name[0]==0 || de->d_name[0]=='.') continue;

    batdirname= de->d_name;
    r= readbattery();
    tidybattery();

    if (r) {
    bad:
      charging_mask |= (1u << CHGST_ERROR);
      break;
    }

    if (this_type == TYPE_BATTERY) {
      if (!this_present)
	continue;

      charging_mask |= 1u << this_state;

#define QTY_SUPPLIED(f,...)   this_##f != VAL_NOTFOUND &&
#define QTY_USE_ENERGY(f,...) this_##f = this_##f##_energy;
#define QTY_USE_CHARGE(f,...) this_##f = this_##f##_charge;

      double funky_multiplier;
      if (BAT_QTYS(QTY_SUPPLIED,_energy,,) 1) {
	if (debug) printf(" using energy\n");
	BAT_QTYS(QTY_USE_ENERGY,,,);
	funky_multiplier = 1.0;
      } else if (BAT_QTYS(QTY_SUPPLIED,_charge,,)
		 this_voltage != VAL_NOTFOUND) {
	if (debug) printf(" using charge\n");
	BAT_QTYS(QTY_USE_CHARGE,,,);
	funky_multiplier = this_voltage * 1e-6;
      } else {
	batfailc("neither complete set of energy nor charge");
	continue;
      }
      if (this_state == CHGST_DISCHARGING)
	/* negate it */
	total_present_rate -= 2.0 * this_present_rate * funky_multiplier;

#define Q_ACCUMULATE_FUNKY(f,...)			\
      total_##f += this_##f * funky_multiplier;
BAT_QTYS(Q_ACCUMULATE_FUNKY,,,)
    }

#define Q_ACCUMULATE_PLAIN(f,t,...)			\
    if (this_type == TYPE_##t)			\
      total_##f += this_##f;
ALL_PLAIN_ACCUMULATE_FIELDS(Q_ACCUMULATE_PLAIN)

      
  }
  if (di) closedir(di);

  if (debug) {
    printf("TOTAL:\n");
    printf(" %-30s = %#20x\n", "mask", charging_mask);
#define T_PRINT(f,...)					\
    printf(" %-30s = %20.6f\n", #f, total_##f);
BAT_QTYS(T_PRINT,,,)
ALL_PLAIN_ACCUMULATE_FIELDS(T_PRINT)
  }

  if ((charging_mask & (1u<<CHGST_DISCHARGING)) &&
      !total_online/*mains*/) {
    double time_remaining =
      -total_remaining_capacity * 3600.0 / total_present_rate;
    if (debug) printf(" %-30s = %20.6f\n", "time remaining", time_remaining);
    if (time_remaining < alarmlevel)
      alarmed = 1;
  }

  if (total_design_capacity < 0.5)
    total_design_capacity= 1.0;

  if (total_last_full_capacity < total_remaining_capacity)
    total_last_full_capacity= total_remaining_capacity;
  if (total_design_capacity < total_last_full_capacity)
    total_design_capacity= total_last_full_capacity;

  nondegraded_norm= total_last_full_capacity / total_design_capacity;
  fill_norm= total_remaining_capacity / total_design_capacity;
  ratepersec_norm=  total_present_rate
    / (3600.0 * total_design_capacity);
}

static void initacquire(void) {
}  

/*---------- argument parsing ----------*/

#define COLOURS					\
  C(blue,           discharging)		\
  C(green,         charging)			\
  C(cyan,           charged)			\
  C(lightgrey,      notcharging)		\
  C(grey,           confusing)			\
  C(black,          normal)			\
  C(red,            low)			\
  C(dimgrey,        degraded)			\
  C(darkgreen,      absent)			\
  C(yellow,         error)			\
  C(white,          equilibrium)		\
  GC(remain)					\
  GC(white)					\
  GC(empty)

static XrmDatabase xrm;
static Display *disp;
static int screen;
static const char *parentwindow;

static const char defaultresources[]=
#define GC(g)
#define C(c,u)					\
  "*" #u "Color: " #c "\n"
  COLOURS
#undef GC
#undef C
  ;

#define S(s) ((char*)(s))
static const XrmOptionDescRec optiontable[]= {
  { S("-debug"),        S("*debug"),        XrmoptionIsArg },
  { S("-warningTime"),  S("*warningTime"),  XrmoptionSepArg },
  { S("-display"),      S("*display"),      XrmoptionSepArg },
  { S("-geometry"),     S("*geometry"),     XrmoptionSepArg },
  { S("-into"),         S("*parentWindow"), XrmoptionSepArg },
  { S("-iconic"),       S("*iconic"),       XrmoptionIsArg },
  { S("-withdrawn"),    S("*withdrawn"),    XrmoptionIsArg },
#define GC(g)
#define C(c,u)							\
  { S("-" #u "Color"),  S("*" #u "Color"),  XrmoptionSepArg },	\
  { S("-" #u "Colour"), S("*" #u "Color"),  XrmoptionSepArg },
  COLOURS
#undef GC
#undef C
};

static const char *getresource(const char *want) {
  char name_buf[256], class_buf[256];
  XrmValue val;
  char *rep_type_dummy;
  int r;

  assert(strlen(want) < 128);

  sprintf(name_buf,"xbatmon-simple.%s",want);
  sprintf(class_buf,"Xbatmon-Simple.%s",want);
  
  r= XrmGetResource(xrm, name_buf,class_buf, &rep_type_dummy, &val);
  if (r) return val.addr;

  sprintf(name_buf,"xacpi-simple.%s",want);
  sprintf(class_buf,"Xacpi-Simple.%s",want);
  
  r= XrmGetResource(xrm, name_buf,class_buf, &rep_type_dummy, &val);
  if (r) return val.addr;
  
  return 0;
}

static int getresource_bool(const char *want, int def, int *cache) {
  /* *cache should be initialised to -1 and will be set to !!value
   * alternatively cache==0 is allowed */

  if (cache && *cache >= 0) return *cache;

  const char *str= getresource(want);
  int result = def;
  if (str && str[0]) {
    char *ep;
    long l= strtol(str,&ep,0);
    if (!*ep) {
      result = l > 0;
    } else {
      switch (str[0]) {
      case 't': case 'T': case 'y': case 'Y':         result= 1;  break;
      case 'f': case 'F': case 'n': case 'N':         result= 0;  break;
      case '-': /* option name from XrmoptionIsArg */ result= 1;  break;
      }
    }
  }

  if (cache) *cache= result;
  return result;
}

static void more_resources(const char *str, const char *why) {
  XrmDatabase more;

  if (!str) return;

  more= XrmGetStringDatabase((char*)str);
  if (!more) fail(why);
  XrmCombineDatabase(more,&xrm,0);
}

static void parseargs(int argc, char **argv) {
  Screen *screenscreen;
  
  XrmInitialize();

  XrmParseCommand(&xrm, (XrmOptionDescRec*)optiontable,
		  sizeof(optiontable)/sizeof(*optiontable),
		  program_name, &argc, argv);

  if (argc>1) badusage();

  getresource_bool("debug",0,&debug);

  const char *alarmlevel_string= getresource("alarmLevel");
  alarmlevel = alarmlevel_string ? atoi(alarmlevel_string) : 300;

  parentwindow = getresource("parentWindow");

  disp= XOpenDisplay(getresource("display"));
  if (!disp) fail("could not open display");

  screen= DefaultScreen(disp);

  screenscreen= ScreenOfDisplay(disp,screen);
  if (!screenscreen) fail("screenofdisplay");
  more_resources(XScreenResourceString(screenscreen), "screen resources");
  more_resources(XResourceManagerString(disp), "display resources");
  more_resources(defaultresources, "default resources");
} 

/*---------- display ----------*/

static Window win;
static int width, height;
static Colormap cmap;
static unsigned long lastbackground;

typedef struct {
  GC gc;
  unsigned long lastfg;
} Gcstate;

#define C(c,u) static unsigned long pix_##u;
#define GC(g) static Gcstate gc_##g;
  COLOURS
#undef C
#undef GC

static void refresh(void);

#define CHGMASK_CHG_DIS ((1u<<CHGST_CHARGING) | (1u<<CHGST_DISCHARGING))

static void failr(const char *m, int r) {
  fprintf(stderr,"error: %s (code %d)\n", m, r);
  exit(-1);
}

static void setbackground(unsigned long newbg) {
  int r;
  
  if (newbg == lastbackground) return;
  r= XSetWindowBackground(disp,win,newbg);
  if (!r) fail("XSetWindowBackground");
  lastbackground= newbg;
}

static void setforeground(Gcstate *g, unsigned long px) {
  XGCValues gcv;
  int r;
  
  if (g->lastfg == px) return;
  
  memset(&gcv,0,sizeof(gcv));
  g->lastfg= gcv.foreground= px;
  r= XChangeGC(disp,g->gc,GCForeground,&gcv);
  if (!r) fail("XChangeGC");
}

static void show_solid(unsigned long px) {
  setbackground(px);
  XClearWindow(disp,win);
}

static void show(void) {
  double elap, then;
  int i, leftmost_lit, leftmost_nondeg, beyond, first_beyond;

  if (!charging_mask)
    return show_solid(pix_absent);

  if (charging_mask & (1u << CHGST_ERROR))
    return show_solid(pix_error);

  setbackground(pix_degraded);
  XClearWindow(disp,win);
  
  setforeground(&gc_remain,
		!(charging_mask & CHGMASK_CHG_DIS) ?
		(~charging_mask & (1u << CHGST_CHARGED) ?
		 pix_notcharging : pix_charged) :
		!(~charging_mask & CHGMASK_CHG_DIS) ? pix_confusing :
		charging_mask & (1u<<CHGST_CHARGING)
		? pix_charging : pix_discharging);
		
  setforeground(&gc_empty, alarmed ? pix_low : pix_normal);

  for (i=0, first_beyond=1; i<height; i++) {
    elap= !i ? 0 :
      height==2 ? BOTTOM :
      TOP * exp( (double)i / (height-2) * log( (double)BOTTOM/TOP ) );
    
    then= fill_norm + ratepersec_norm * elap;

    beyond=
      ((charging_mask & (1u<<CHGST_DISCHARGING) && then <= 0.0) ||
       (charging_mask & (1u<<CHGST_CHARGING) && then>=nondegraded_norm));

    if (then <= 0.0) then= 0.0;
    else if (then >= nondegraded_norm) then= nondegraded_norm;

    leftmost_lit= width * then;
    leftmost_nondeg= width * nondegraded_norm;

    if (beyond && first_beyond) {
      XDrawLine(disp, win, gc_white.gc, 0,i, leftmost_nondeg,i);
      first_beyond= 0;
    } else {
      if (leftmost_lit < leftmost_nondeg)
	XDrawLine(disp, win, gc_empty.gc,
		  leftmost_lit,i, leftmost_nondeg,i);
      if (leftmost_lit >= 0)
	XDrawLine(disp, win, gc_remain.gc, 0,i, leftmost_lit,i);
    }
  }
}

static void initgc(Gcstate *gc_r) {
  XGCValues gcv;

  memset(&gcv,0,sizeof(gcv));
  gcv.function= GXcopy;
  gcv.line_width= 1;
  gc_r->lastfg= gcv.foreground= pix_equilibrium;
  gc_r->gc= XCreateGC(disp,win, GCFunction|GCLineWidth|GCForeground, &gcv);
}

static void colour(unsigned long *pix_r, const char *whichcolour) {
  XColor xc;
  const char *name;
  Status st;

  name= getresource(whichcolour);
  if (!name) fail("get colour resource");
  
  st= XAllocNamedColor(disp,cmap,name,&xc,&xc);
  if (!st) fail(name);
  
  *pix_r= xc.pixel;
}

static void initgraphics(int argc, char **argv) {
  int xwmgr, r;
  const char *geom_string;
  XSizeHints *normal_hints;
  XWMHints *wm_hints;
  XClassHint *class_hint;
  int pos_x, pos_y, gravity;
  char *program_name_silly;
  
  program_name_silly= (char*)program_name;

  normal_hints= XAllocSizeHints();
  wm_hints= XAllocWMHints();
  class_hint= XAllocClassHint();

  if (!normal_hints || !wm_hints || !class_hint)
    fail("could not alloc hint(s)");

  geom_string= getresource("geometry");

  xwmgr= XWMGeometry(disp,screen, geom_string,"128x32", 0,
		 normal_hints,
		 &pos_x, &pos_y,
		 &width, &height,
		 &gravity);

  unsigned long parentwindowid;
  if (parentwindow)
    parentwindowid = strtoul(parentwindow,0,0);
  else
    parentwindowid = DefaultRootWindow(disp);

  win= XCreateSimpleWindow(disp,parentwindowid,
			   pos_x,pos_y,width,height,0,0,0);
  cmap= DefaultColormap(disp,screen);
  
#define C(c,u) colour(&pix_##u, #u "Color");
#define GC(g) initgc(&gc_##g);
  COLOURS
#undef C
#undef GC

  r= XSetWindowBackground(disp,win,pix_degraded);
  if (!r) fail("init set background");
  lastbackground= pix_degraded;

  normal_hints->flags= PWinGravity;
  normal_hints->win_gravity= gravity;
  normal_hints->x= pos_x;
  normal_hints->y= pos_y;
  normal_hints->width= width;
  normal_hints->height= height;
  if ((xwmgr & XValue) || (xwmgr & YValue))
    normal_hints->flags |= USPosition;

  wm_hints->flags= InputHint;
  wm_hints->input= False;
  wm_hints->initial_state=
    (getresource_bool("withdrawn",0,0) ? WithdrawnState :
     getresource_bool("iconic",0,0) ? IconicState
     : NormalState);

  class_hint->res_name= program_name_silly;
  class_hint->res_class= program_name_silly;

  XmbSetWMProperties(disp,win, program_name,program_name,
		     argv,argc, normal_hints, wm_hints, class_hint);

  XSelectInput(disp,win, ExposureMask|StructureNotifyMask);
  XMapWindow(disp,win);
}
 
static void refresh(void) {
  acquiredata();
  show();
}

static void newgeometry(void) {
  int dummy;
  Window dummyw;
  
  XGetGeometry(disp,win, &dummyw,&dummy,&dummy, &width,&height, &dummy,&dummy);
}

static void eventloop(void) {
  XEvent ev;
  struct pollfd pfd;
  int r, timeout;
  
  newgeometry();
  refresh();

  for (;;) {
    XFlush(disp);

    pfd.fd= ConnectionNumber(disp);
    pfd.events= POLLIN|POLLERR;

    timeout= !(charging_mask & (1u << CHGST_ERROR)) ? TIMEOUT : TIMEOUT_ONERROR;
    r= poll(&pfd,1,timeout);
    if (r==-1 && errno!=EINTR) failr("poll",errno);

    while (XPending(disp)) {
      XNextEvent(disp,&ev);
      if (ev.type == ConfigureNotify) {
	XConfigureEvent *ce= (void*)&ev;
	width= ce->width;
	height= ce->height;
      }
    }
    refresh();
  }
}

int main(int argc, char **argv) {
  parseargs(argc,argv);
  initacquire();
  initgraphics(argc,argv);
  eventloop();
  return 0;
}
