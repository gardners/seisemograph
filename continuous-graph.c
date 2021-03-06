#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <dirent.h>
#include <assert.h>
#include <fcntl.h>
#include <time.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xos.h>

#include <sys/time.h>

#define PNG_DEBUG 3
#include <png.h>

Display *dis;
int screen;
Window win;
GC gc;
unsigned long black,white, red, green, blue;

#define MAXX (1920-64)
#define MAXY (1200-32)
unsigned char frame[MAXY][MAXX*4];

int frame_count=0;

// X,Y,Z samples every second
#define MAX_HISTORY 86400
unsigned int recent_data[MAX_HISTORY][3];

int minx,miny,minz;
int maxx,maxy,maxz;
int meanx,meany,meanz;
  
int image_number=0;

void write_image(char *filename);

ssize_t read_nonblock(int fd, void *buf, size_t len);
ssize_t write_all(int fd, const void *buf, size_t len);
int serial_setup_port_with_speed(int fd,int speed);
void read_png_file(char* file_name);
int draw_text(int x,int y,char *s,int colour);

char *png_file=NULL;

int width, height;
png_byte color_type;
png_byte bit_depth;

png_structp png_ptr;
png_infop info_ptr;
int number_of_passes;
png_bytep * row_pointers;
FILE *infile;

int x11_handler(Display *d, XErrorEvent *e)
{
  printf("Error with display\n");  
  return 0;
}

void init_x() {
  /* get the colors black and white (see section for details) */

  /* use the information from the environment variable DISPLAY 
     to create the X connection:
  */	
  dis=XOpenDisplay(":0");
  screen=DefaultScreen(dis);
  black=BlackPixel(dis,screen);	/* get color black */
  white=WhitePixel(dis, screen);  /* get color white */
  XColor tmp;
  XParseColor(dis, DefaultColormap(dis,screen), "red", &tmp);
  XAllocColor(dis,DefaultColormap(dis,screen),&tmp);
  red=tmp.pixel;
  XParseColor(dis, DefaultColormap(dis,screen), "green", &tmp);
  XAllocColor(dis,DefaultColormap(dis,screen),&tmp);
  green=tmp.pixel;
  XParseColor(dis, DefaultColormap(dis,screen), "blue", &tmp);
  XAllocColor(dis,DefaultColormap(dis,screen),&tmp);
  blue=tmp.pixel;

  XSetErrorHandler(x11_handler);
  
  /* once the display is initialized, create the window.
     This window will be have be 200 pixels across and 300 down.
     It will have the foreground white and background black
  */
  win=XCreateSimpleWindow(dis,RootWindow(dis,screen),
			  0,0,	
			  MAXX,MAXY,
			  1, white,
			  black);
  
  /* here is where some properties of the window can be set.
     The third and fourth items indicate the name which appears
     at the top of the window and the name of the minimized window
     respectively.
  */
  XSetStandardProperties(dis,win,"XSeismo","XSeismo",None,NULL,0,NULL);

  /* this routine determines which types of input are allowed in
	   the input.  see the appropriate section for details...
  */
  XSelectInput(dis, win, ExposureMask);
  
  /* create the Graphics Context */
  gc=XCreateGC(dis, win, 0,0);        
  
  /* here is another routine to set the foreground and background
     colors _currently_ in use in the window.
  */
  XSetBackground(dis,gc,black);
  XSetForeground(dis,gc,white);
  
  /* clear the window and bring it on top of the other windows */
  XClearWindow(dis, win);
  XMapWindow(dis, win);
};

float absf(float f)
{
  if (f<0) return -f;
  return f;
}

int x_setcol(int colour)
{
  int xcol=white;
  if (colour==0xffffff) xcol=white;
  if (colour==0xff0000) xcol=blue;
  if (colour==0x00ff00) xcol=green;
  if (colour==0x0000ff) xcol=red;
  XSetForeground(dis,gc,xcol);
  return 0;
}

int update_image(void)
{
  int x,y;
  
  // Clear PNG output frame (make all black, but with alpha)
  for(y=0;y<MAXY;y++)    
    for(x=0;x<MAXX*4;x++)
      if ((x&3)==3) frame[y][x]=0xff; else frame[y][x]=0x00;
  
  XSetBackground(dis,gc,white);
  XSetForeground(dis,gc,black);
  XFillRectangle(dis,win,gc,0,0,MAXX,MAXY);

  // Work out most recent sample
  int head=time(0)%MAX_HISTORY;

  // Find recent major excursions
  float maxxdelta=0;
  float maxydelta=0;
  float maxzdelta=0;
  float maxdelta=0;
  int maxpoint=-1;

#define MAX_EXCURSIONS 10
  int recent_excursions[MAX_EXCURSIONS];
  int recent_excursion_count=0;

  draw_text(64,32+0*MAXY/4,"Coulthard's Lookout Seismograph.",0xffffff);
  draw_text(64,48+0*MAXY/4,"Last 3 minutes:",0xffffff);
  draw_text(64,0+1*MAXY/4,"Last 3 hours:",0xffffff);
  draw_text(64,0+2*MAXY/4,"Last 24 hours:",0xffffff);
  draw_text(64,-16+1*MAXY/4,"Colours are for X, Y and Z axes.",0xffffff);
  draw_text(64+16*16,-16+1*MAXY/4,"X",0x0000ff);
  draw_text(64+19*16,-16+1*MAXY/4,"Y",0x00ff00);
  draw_text(64+25*16,-16+1*MAXY/4,"Z",0xff0000);
    
  for(int s=0;s<MAX_HISTORY;s++) {
    int sn=head-s;
    if (sn<0) sn+=MAX_HISTORY;
    if (recent_data[sn][0]) {
      float xdelta=absf((recent_data[sn][0]-minx)*1.0/meanx);
      float ydelta=absf((recent_data[sn][1]-miny)*1.0/meany);
      float zdelta=absf((recent_data[sn][2]-minz)*1.0/meanz);
      float delta=xdelta+ydelta+zdelta;
      if (0) printf("sn=%d, sample=%d,%d,%d, deltas=%f,%f,%f\n",
		    sn,
		    recent_data[sn][0],
		    recent_data[sn][1],
		    recent_data[sn][2],
		    xdelta,ydelta,zdelta);
      if (delta>maxdelta) {
	// printf("sn=%d, delta=%f, maxdelta=%f\n",sn,delta,maxdelta);
	if ((maxpoint>-1)&&
	    (((sn-maxpoint)>500)
	     ||((maxpoint-sn)>500))
	    ) {
	  // We are replacing recent excursion(s)
	  for(int i=MAX_EXCURSIONS-1;i>0;i--)
	    recent_excursions[i]=recent_excursions[i-1];
	  if (recent_excursion_count<MAX_EXCURSIONS)
	    recent_excursion_count++;
	  recent_excursions[0]=maxpoint;
	}

      maxdelta=delta;
	maxpoint=sn;
      }
    }
  }
  
  // Draw 3 minute second-by-second log
  for(int chan=0;chan<3;chan++) {
    int lasty=-1;
    for(int i=-181;i<0;i++) {
      int sample=head+i;
      if (sample<0) sample+=86400;
      
      // Work out pixel translation
      int x=MAXX+i*MAXX/180;
      int x1=MAXX+(i+1)*MAXX/180;
      if (x>=MAXX) x=MAXX-1;
      if (x1<0) x1=0;
      int ylo=0+16;
      int yhi=MAXY/4-16;
      int min;
      int range=0;
      switch(chan) {
      case 0: range=maxx-minx; min=minx; break;
      case 1: range=maxy-miny; min=miny; break;
      case 2: range=maxz-minz; min=minz; break;
      }
      
      int y=ylo+1.0*(recent_data[sample][chan]-min)*(yhi-ylo)/range;
      if (0) 
	printf("ylo=%d, yhi=%d, scale=%f, min,data,max=%d,%d,%d, scaled=%d, x=%d, x1=%d\n",
	       ylo,yhi,1.0*(yhi-ylo)/range,
	       minx,recent_data[sample][0],maxx,
	       y,x,x1);
      if (y<0||y>MAXY) y=(ylo+yhi)/2;

      if (!lasty||(lasty==-1)) lasty=y;

      float slope=1.0*(y-lasty)/(x1-x);

      if (i>-179) {
	int base=lasty;
	for(int xx=x;xx<x1;xx++) {
	  int they=base+slope*(xx-x);
	  for(int yy=they;yy<they+3;yy++) {
	    frame[yy][xx*4+chan]=0xff;
	    x_setcol((frame[yy][xx*4+0]<<0)
		     +(frame[yy][xx*4+1]<<8)
		     +(frame[yy][xx*4+2]<<16));
	    XDrawPoint(dis,win,gc,xx,yy);
	  }
	}
	lasty=y;
      }
    }
    
  }
  
  // Draw 3 hour log
  for(int chan=0;chan<3;chan++) {
    int duration=3600*3;
    int ylo=1*MAXY/4+16;
    int yhi=2*MAXY/4-16;

    for(int i=-(duration+1);i<=0;i++) {
      int sample=head+i;
      if (sample<0) sample+=86400;
      
      // Work out pixel translation
      int x=MAXX+i*MAXX/duration;
      int x1=MAXX+(i+1)*MAXX/duration;
      if (x>=MAXX) x=MAXX-1;
      if (x1<0) x1=0;
      int min;
      int range=0;
      switch(chan) {
      case 0: range=maxx-minx; min=minx; break;
      case 1: range=maxy-miny; min=miny; break;
      case 2: range=maxz-minz; min=minz; break;
      }

      int y=ylo+1.0*(recent_data[sample][chan]-min)*(yhi-ylo)/range;
      if (0) 
	printf("ylo=%d, yhi=%d, scale=%f, min,data,max=%d,%d,%d, scaled=%d, x=%d, x1=%d\n",
	       ylo,yhi,1.0*(yhi-ylo)/range,
	       minx,recent_data[sample][0],maxx,
	       y,x,x1);
      if (y<0||y>MAXY) y=(ylo+yhi)/2;

      // Draw pixels
      frame[y][x*4+chan]=0xff;
      x_setcol((frame[y][x*4+0]<<0)
	       +(frame[y][x*4+1]<<8)
	       +(frame[y][x*4+2]<<16));
      XDrawPoint(dis,win,gc,x,y);
    }
    
  }
  
  // Draw 24 hour log
  for(int chan=0;chan<3;chan++) {
    int duration=86400;
    int ylo=2*MAXY/4+16;
    int yhi=3*MAXY/4-16;

    for(int i=-(duration+1);i<=0;i++) {
      int sample=head+i;
      if (sample<0) sample+=86400;
      
      // Work out pixel translation
      int x=MAXX+i*MAXX/duration;
      int x1=MAXX+(i+1)*MAXX/duration;
      if (x>=MAXX) x=MAXX-1;
      if (x1<0) x1=0;
      int min;
      int range=0;
      switch(chan) {
      case 0: range=maxx-minx; min=minx; break;
      case 1: range=maxy-miny; min=miny; break;
      case 2: range=maxz-minz; min=minz; break;
      }

      int y=ylo+1.0*(recent_data[sample][chan]-min)*(yhi-ylo)/range;
      if (0) 
	printf("ylo=%d, yhi=%d, scale=%f, min,data,max=%d,%d,%d, scaled=%d, x=%d, x1=%d\n",
	       ylo,yhi,1.0*(yhi-ylo)/range,
	       minx,recent_data[sample][0],maxx,
	       y,x,x1);
      if (y<0||y>MAXY) y=(ylo+yhi)/2;

      // Draw pixels
      frame[y][x*4+chan]=0xff;
      x_setcol((frame[y][x*4+0]<<0)
	       +(frame[y][x*4+1]<<8)
	       +(frame[y][x*4+2]<<16));
      XDrawPoint(dis,win,gc,x,y);
    }
    
  }
  
  // Draw most recent excursion, starting 30 seconds before
  int time_delta=head-maxpoint;
  if (time_delta<0) time_delta+=MAX_HISTORY;
  printf("Drawing excursion from T-%dsec\n",
	 time_delta);
  char recent_msg[1024];
  snprintf(recent_msg,1024,"Most significant trace in last 24 hours (peak %02d:%02d.%02d ago)",
	   (time_delta)/3600,
	   ((time_delta)/60)%60,
	   (time_delta)%60);
  draw_text(64,0+3*MAXY/4,recent_msg,0xffffff);
  
  for(int chan=0;chan<3;chan++) {
    int duration=300;
    int ylo=3*MAXY/4+16;
    int yhi=4*MAXY/4-16;

    int lasty=-1;
    for(int i=-(duration+2);i<=0;i++) {
      //  Place max point 30 seconds from left
      int sample=head+(maxpoint-head)+(duration-60)+i;
      if (sample<0) sample+=86400;
      
      // Work out pixel translation
      int x=MAXX+i*MAXX/duration;
      int x1=MAXX+(i+1)*MAXX/duration;
      if (x>=MAXX) x=MAXX-1;
      if (x1<0) x1=0;
      int min;
      int range=0;
      switch(chan) {
      case 0: range=maxx-minx; min=minx; break;
      case 1: range=maxy-miny; min=miny; break;
      case 2: range=maxz-minz; min=minz; break;
      }

      int y=ylo+1.0*(recent_data[sample][chan]-min)*(yhi-ylo)/range;
      if (0) 
	printf("ylo=%d, yhi=%d, scale=%f, min,data,max=%d,%d,%d, scaled=%d, x=%d, x1=%d\n",
	       ylo,yhi,1.0*(yhi-ylo)/range,
	       minx,recent_data[sample][0],maxx,
	       y,x,x1);
      if (y<0||y>MAXY) y=(ylo+yhi)/2;

      if (!lasty||(lasty==-1)) lasty=y;

      float slope=1.0*(y-lasty)/(x1-x);

      if (i>(-duration)) {
	int base=lasty;
	for(int xx=x;xx<x1;xx++) {
	  int they=base+slope*(xx-x);
	  for(int yy=they;yy<they+3;yy++) {
	    frame[yy][xx*4+chan]=0xff;
	    x_setcol((frame[yy][xx*4+0]<<0)
		      +(frame[yy][xx*4+1]<<8)
			+(frame[yy][xx*4+2]<<16));
	    XDrawPoint(dis,win,gc,xx,yy);
	  }
	}
	lasty=y;
      }
    }
    
  }
  


  return 0;
}

int process_line(char *line)
{
  int e,n,v,x,y,z;

  //  if (line[0])  printf("Read '%s'\n",line);

  if (sscanf(line,"Minimum %d %d %d %d %d %d",
	     &e,&n,&v,&minx,&miny,&minz)==6) {
    // printf("Read minimums\n");
  }
  if (sscanf(line,"Maximum %d %d %d %d %d %d",
	     &e,&n,&v,&maxx,&maxy,&maxz)==6) {
    // printf("Read maximums %d %d %d\n",maxx,maxy,maxz);
  }
  sscanf(line,"Mean %d %d %d %d %d %d",
	 &e,&n,&v,&meanx,&meany,&meanz);
  
  
  if (sscanf(line,"Current %d %d %d %d %d %d",
	     &e,&n,&v,&x,&y,&z)==6) {
    // Got a sample
    int sample_number=time(0)%MAX_HISTORY;
    recent_data[sample_number][0]=x;
    recent_data[sample_number][1]=y;
    recent_data[sample_number][2]=z;
    
    fprintf(stderr,"Sample #%d = %d,%d,%d\n",
	    sample_number,x,y,z);
    
    
    update_image();
    frame_count++;
    if (frame_count==60) 
      {
	write_image("/tmp/tmp.png");
	rename("/tmp/tmp.png",png_file);
	frame_count=0;
      } else frame_count++;
  }
  return 0;
}

int main(int argc,char **argv)
{
  time_t last_time=0;

  if (argc!=4) {
    fprintf(stderr,"usage: continuous-graph <serial port> <png file> <font file>\n");
    exit(-3);
  }

  png_file=argv[2];
  
  printf("Opening serial port '%s'\n",argv[1]);
  
  int seismo=open(argv[1],O_RDWR);

  printf("Setting up serial port.\n");
  serial_setup_port_with_speed(seismo,38400);

  read_png_file(argv[3]);
  
  
  char line[1024];

  printf("Clearing data...\n");

  init_x();  
  
  // Clear data history also
  int x,y;
  for(x=0;x<MAX_HISTORY;x++) {
    for(y=0;y<3;y++) recent_data[x][y]=0;
  }
  
  printf("Reading data\n");

  line[0]=0;
  int len=0;
  while(1) {
    XEvent e;
    if (XCheckWindowEvent(dis, win, ExposureMask, &e)) {
	if (e.type == Expose) 
	  {
	    printf("Expose event\n");
	    update_image();
	  }
      }
    unsigned char buf[1024];
    int r=read_nonblock(seismo,buf,1024);
    if (r<1) usleep(10000);
    if (r>0) {
      if (0) {
	//	printf("%d bytes read.\n",r);
	for(int i=0;i<r;i++)
	  printf("%02x '%c'\n",(unsigned)line[len+i],(unsigned)line[len+i]);
      }
      for(int i=0;i<r;i++)
	if (buf[i]=='\r'||buf[i]=='\n') {
	  line[len]=0;
	  process_line(line);
	  line[0]=0;
	  len=0;
	  // Check if freewave box has just been rebooted
	  if (strstr(line,"accept")) write_all(seismo,"a\r",2);
  
	} else
	  line[len++]=buf[i];
    }
    
    // Request next sample
    if (time(0)!=last_time) {
      printf("Asking for next sample.\n");
      write_all(seismo,"view signal\r",4+1+6+1);
      last_time=time(0);
    }
    
  }
  return 0;
}
  
void write_image(char *filename)
{
  int y;
  png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING,NULL,NULL,NULL);
  if (!png) abort();

  png_infop info = png_create_info_struct(png);
  if (!info) abort();

  if (setjmp(png_jmpbuf(png))) abort();

  FILE *f=fopen(filename,"wb");
  if (!f) abort();

  png_init_io(png,f);

  png_set_IHDR(
	       png,
	       info,
	       MAXX,MAXY,
	       8,
	       PNG_COLOR_TYPE_RGBA,
	       PNG_INTERLACE_NONE,
	       PNG_COMPRESSION_TYPE_BASE,
	       PNG_FILTER_TYPE_DEFAULT
	       );

  png_write_info(png,info);

  for(y=0;y<MAXY;y++) {
    png_write_row(png,frame[y]);
  }

  png_write_end(png,info);
  png_destroy_write_struct(&png, &info);
  
  fclose(f);
  
  return;
}

void abort_(const char * s, ...)
{
  va_list args;
  va_start(args, s);
  vfprintf(stderr, s, args);
  fprintf(stderr, "\n");
  va_end(args);
  abort();
}

/* ============================================================= */

void read_png_file(char* file_name)
{
  unsigned char header[8];    // 8 is the maximum size that can be checked

  /* open file and test for it being a png */
  infile = fopen(file_name, "rb");
  if (infile == NULL)
    abort_("[read_png_file] File %s could not be opened for reading", file_name);

  int r=fread(header, 1, 8, infile);
  if ((r<8)||png_sig_cmp(header, 0, 8))
    abort_("[read_png_file] File %s is not recognized as a PNG file", file_name);

  /* initialize stuff */
  png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);

  if (!png_ptr)
    abort_("[read_png_file] png_create_read_struct failed");

  info_ptr = png_create_info_struct(png_ptr);
  if (!info_ptr)
    abort_("[read_png_file] png_create_info_struct failed");

  if (setjmp(png_jmpbuf(png_ptr)))
    abort_("[read_png_file] Error during init_io");

  png_init_io(png_ptr, infile);
  png_set_sig_bytes(png_ptr, 8);

  // Convert palette to RGB values
  png_set_expand(png_ptr);

  png_read_info(png_ptr, info_ptr);

  width = png_get_image_width(png_ptr, info_ptr);
  height = png_get_image_height(png_ptr, info_ptr);
  color_type = png_get_color_type(png_ptr, info_ptr);
  bit_depth = png_get_bit_depth(png_ptr, info_ptr);

  printf("Input-file is: width=%d, height=%d.\n", width, height);

  number_of_passes = png_set_interlace_handling(png_ptr);
  png_read_update_info(png_ptr, info_ptr);

  /* read file */
  if (setjmp(png_jmpbuf(png_ptr)))
    abort_("[read_png_file] Error during read_image");

  row_pointers = (png_bytep*) malloc(sizeof(png_bytep) * height);
  for (int y=0; y<height; y++)
    row_pointers[y] = (png_byte*) malloc(png_get_rowbytes(png_ptr,info_ptr));

  png_read_image(png_ptr, row_pointers);

  if (infile != NULL) {
    fclose(infile);
    infile = NULL;
  }

  printf("Input-file is read and now closed\n");
}

int draw_char(int x,int y,int c,int colour)
{
  x_setcol(colour);
  
  for(int xx=0;xx<16;xx++)
    for(int yy=0;yy<16;yy++)
      {
	if (row_pointers[(c*8)+(yy/2)][(xx/2)*4]) {
	  frame[y+yy][(x+xx)*4+0]=(colour>>0)&0xff;
	  frame[y+yy][(x+xx)*4+1]=(colour>>8)&0xff;
	  frame[y+yy][(x+xx)*4+2]=(colour>>16)&0xff;

	  XDrawPoint(dis,win,gc,x+xx,y+yy);
	}
      }
  return 0;
}

int draw_text(int x,int y,char *s,int colour)
{
  int xx=x,yy=y;
  for(int i=0;s[i];i++) {
    switch(s[i]) {
    case '\r':
      xx=0;
      break;
    case '\n':
      yy+=16;
      break;
    default:
      draw_char(xx,yy,s[i],colour);
      xx+=16; if (xx>=MAXX) { xx=0; yy+=16; }
      if (yy>MAXY) yy=0;
      break;
    }
  }
  return 0;
}
