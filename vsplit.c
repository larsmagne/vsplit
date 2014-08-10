#include <stdio.h>
#include <getopt.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <strings.h>

#define BUFFER_SIZE 0x200000

int limit = 20;
char *output = "output";
int avglen = 1000;
int verbose = 0;
int spread = 1000;

void sp_write (int fd, void *buf, int count) {
  int l, start = 0, block;

  while (count > 0) {
    block = BUFFER_SIZE;
    if (block > count) 
      block = count;
    if ((l = write (fd, (char*)(buf + start), block)) < 0) {
      perror ("vsplit write");
      exit (-1);
    }
    start += l;
    count -= l;
  }
}						   


void chop (int in, int start, int stop, int part) {
  short *buf;
  int count = start;
  int max = BUFFER_SIZE * sizeof(short);
  char *sbuf = (char*)malloc(1000);
  int out, len;

  fprintf (stderr, "%d: Chopping from %d to %d.\n", part, start, stop);
  
  if (! (buf = (short *) malloc (BUFFER_SIZE * sizeof(short)))) {
    perror ("vsplit");
    exit (-1);
  }

  if (lseek (in, start, SEEK_SET) < 0) {
    perror ("vsplit");
    exit (-1);
  }
    
  sprintf (sbuf, "%s-%02d.raw", output, part);
  if ((out = open (sbuf, O_RDWR | O_CREAT | O_TRUNC, 0644)) < 0) {
    perror ("vsplit");
    exit (-1);
  }

  while (count < stop) {
    len = read (in, buf, (max < stop - count? max: stop - count));
    count += len;
    sp_write (out, buf, len);
  }

  close (out);
  free (sbuf);
  free (buf);
}

void split (char *file) {
  short *buf, *avgbuf;
  int avgb = 0;
  int in, len, in_song = 0, b;
  float avg = 0;
  int count = 0, close_pos = 0, end_pos = 0, split_pos = 0, start_pos = 0;
  int gracelen = 44100 * 2 * 2 * 1.5, i;
  int seq = -1, below = 0;
  int stop[1000], start[1000];
  
  if (! (buf = (short *) malloc (BUFFER_SIZE * sizeof(short)))) {
    perror ("vsplit");
    exit (-1);
  }
  avgbuf = (short *) malloc (avglen * sizeof(short));
  bzero (avgbuf, avglen * sizeof(short));
  
  fprintf (stderr, "Opening file %s...\n", file);
  fprintf (stderr,
	   "Avg buf: %d; limit: %d; spread: %d\n", avglen, limit, spread);
  
  if (! (in = open (file, O_RDONLY))) {
    perror ("vsplit");
    exit (-1);
  }

  while ((len = read (in, buf, BUFFER_SIZE * sizeof(short))) > 0) {
    b = 0;
    len /= 2;
    while (b < len) {
      avg -= (float)(abs(avgbuf[avgb+1]-avgbuf[avgb])) / avglen;
      avgbuf[avgb++] = buf[b];
      avg += (float)(abs(avgbuf[avgb-1]-avgbuf[avgb-2])) / avglen;

      if (avgb >= avglen)
	avgb = 0;

      if (! (count % (44100*2*2)))
	fprintf (stderr, "%d: %.2f\n", count/(44100*2*2), avg);

      count += 4;

      if (in_song) {
	if (avg < limit - spread) {
	  if (below >= 44100 / 4) {
	    in_song = 0;
	    close_pos = 0;
	  } else {
	    if (! end_pos) {
	      fprintf (stderr, "*** End found: %ds. %.2f %d ***\n",
		       count / (44100 * 2 * 2), avg, below);
	      end_pos = count;
	    }
	    below++;
	  }
	} else {
	  end_pos = 0;
	  below = 0;
	}
      } else {
	if (end_pos && (! close_pos) &&
	    (count - gracelen > end_pos ||
	     avg > limit)) {
	  fprintf (stderr, "*** Closing track: %ds. ***\n",
		   count / (44100 * 2 * 2));
	  close_pos = count;
	}
	if (avg > limit + spread) {
	  in_song = 1;
	  start_pos = count;
	  fprintf (stderr, "*** Start found: %ds. ***\n",
		   count / (44100 * 2 * 2));
	  /* Compute split_pos based on start_pos, end_pos and close_pos. */
	  if (close_pos) {
	    /* We have found the closing point. */
	    if (close_pos < start_pos - gracelen) {
	      split_pos = start_pos - gracelen;
	      stop[seq] = close_pos;
	    } else {
	      split_pos = start_pos - (start_pos - end_pos) / 2;
	      stop[seq] = split_pos;
	    }
	  } else if (end_pos) {
	    split_pos = start_pos - (start_pos - end_pos) / 2;
	    stop[seq] = split_pos;
	  } else {
	    /* First track. */
	    split_pos = (start_pos - gracelen > 0?
			 start_pos - gracelen: 0);
	  } 
	  seq++;
	  start[seq] = split_pos;
	  end_pos = 0;
	  close_pos = 0;
	}
      } 
      b += 2;
    }
  }
  if (len == -1) {
    perror ("vsplit");
    exit (-1);
  }

  /* Close the final track. */
  if (close_pos) {
    stop[seq] = close_pos;
  } else if (end_pos) {
    stop[seq] = (end_pos + gracelen > count? count: end_pos + gracelen);
  } else {
    stop[seq] = count;
  }

  for (i=0; i <= seq; i++) 
    chop (in, start[i], stop[i], i+1);

  printf ("%d %d %d\n", i+1, start[i], stop[i]);
  
}

void chop_loop (char *file, char *split) {
  int in, part = 0;
  char *s;
  
  if (! (in = open (file, O_RDONLY))) {
    perror ("vsplit");
    exit (-1);
  }

  s = strtok (split, ",");
  while (s) {
    chop (in, atoi (s), atoi (strchr (s, '.') + 1), part++);
    s = strtok (NULL, ",");
  }
}


int main (int argc, char **argv) {
  int c;
  char *file, *chop_arg = 0;
  
  while (1) {
    int option_index = 0;
    static struct option long_options[] = {
      {"help", 1, 0, 'h'},
      {"floor", 1, 0, 'f'},
      {"length", 1, 0, 'l'},
      {"output", 1, 0, 'o'},
      {"verbose", 1, 0, 'v'},
      {"spread", 1, 0, 's'},
      {"chop", 1, 0, 'c'},
      {0, 0, 0, 0}
    };

    c = getopt_long (argc, argv, "hvl:o:f:s:c:", long_options, &option_index);
    if (c == -1)
      break;

    switch (c) {
    case 'h':
      printf ("\
Usage: vsplit [--help] [--floor <limit>] [--length <length>]
              [--output <output_file>] <file>\n");
      break;

    case 'f':
      limit = atoi (optarg);
      break;

    case 's':
      spread = atoi (optarg);
      break;

    case 'l':
      avglen = atof (optarg) * 44100 * 2;
      break;

    case 'o':
      output = optarg;
      break;

    }
  }

  if (! argv[optind]) {
    printf ("Usage: vsplit <file>\n");
    exit (-1);
  } else {
    file = argv[optind];
  }

  if (chop_arg)
    chop_loop (file, chop_arg);
  else 
    split (file);
  
  return (0);
}
  
