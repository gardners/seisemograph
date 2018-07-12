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
#include <sys/time.h>

#define PNG_DEBUG 3
#include <png.h>

#define MAXX 1024
#define MAXY 768
unsigned char frame[MAXY][MAXX*4];

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

char *png_file=NULL;

int update_image(void)
{
  int x,y;
  
  // Clear PNG output frame (make all black, but with alpha)
  for(y=0;y<MAXY;y++)    
    for(x=0;x<MAXX*4;x++)
      if ((x&3)==3) frame[y][x]=0xff; else frame[y][x]=0x00;

  // Work out most recent sample
  int head=time(0)%MAX_HISTORY;

  // Draw 3 minute second-by-second log
  for(int chan=0;chan<3;chan++) {
    int lasty=-1;
    for(int i=-179;i<=0;i++) {
      int sample=head+i;
      if (sample<0) sample+=86400;
      
      // Work out pixel translation
      int x=MAXX+i*MAXX/180;
      int x1=MAXX+(i+1)*MAXX/180;
      if (x>=MAXX) x=MAXX-1;
      if (x1<0) x1=0;
      int ylo=0;
      int yhi=MAXY/3;
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

      float slope=1.0*(y-lasty)/(x1-x);

      if (1) {
	int thelasty=y;
	int base=lasty;
	for(int xx=x;xx<x1;xx++) {
	  int they=base+slope*(xx-x);
	  for(int yy=thelasty;yy<they+3;yy++)
	    frame[yy][xx*4+chan]=0xff;
	  thelasty=they;
	}
      }
      else {
	// Draw pixels
	for(int xx=x;xx<x1;xx++)
	  for(int yy=0;yy<4;yy++)
	    frame[y+yy][xx*4+chan]=0xff;
      }
      lasty=y;
    }
    
  }
  
  return 0;
}

int process_line(char *line)
{
  int e,n,v,x,y,z;

  if (line[0])  printf("Read '%s'\n",line);
  
  if (sscanf(line,"Minimum %d %d %d %d %d %d",
	     &e,&n,&v,&minx,&miny,&minz)==6) {
    printf("Read minimums\n");
  }
  if (sscanf(line,"Maximum %d %d %d %d %d %d",
	     &e,&n,&v,&maxx,&maxy,&maxz)==6) {
    printf("Read maximums %d %d %d\n",maxx,maxy,maxz);
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
    write_image(png_file);
    
  }
  return 0;
}

int main(int argc,char **argv)
{
  time_t last_time=0;

  
  if (argc!=3) {
    fprintf(stderr,"usage: continuous-graph <serial port> <png file>\n");
    exit(-3);
  }

  png_file=argv[2];
  
  printf("Opening serial port '%s'\n",argv[1]);
  
  int seismo=open(argv[1],O_RDWR);

  printf("Setting up serial port.\n");
  serial_setup_port_with_speed(seismo,38400);
  
  
  char line[1024];

  printf("Clearing data...\n");

  // Clear data history also
  int x,y;
  for(x=0;x<MAX_HISTORY;x++) {
    for(y=0;y<3;y++) recent_data[x][y]=0;
  }
  
  printf("Reading data\n");

  line[0]=0;
  int len=0;
  while(1) {
    unsigned char buf[1024];
    int r=read_nonblock(seismo,buf,1024);
    if (r>0) {
      if (0) {
	//	printf("%d bytes read.\n",r);
	for(int i=0;i<r;i++)
	  printf("%02x '%c'\n",(unsigned)line[len+i],(unsigned)line[len+i]);
      }
      for(int i=0;i<r;i++)
	if (buf[i]=='\r'||buf[i]=='\n') {
	  line[len]=0; process_line(line); line[0]=0; len=0;
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
