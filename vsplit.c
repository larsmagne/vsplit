/*
 * vsplit -- split one sampled vinyl record side into tracks.
 *
 * Input is headerless 44.1kHz 16-bit stereo (host byte order), one record
 * side per file.  You say how many tracks there are; vsplit decides where
 * the boundaries most plausibly are.
 *
 * Pass 1 reads the whole file and reduces it to a loudness envelope (one
 * value per 20ms).  From that it works out:
 *
 *   - the lead-in: silence and needle-drop noise before the music starts,
 *   - the run-out: stylus noise / silence after the music ends,
 *   - the N-1 best places to cut the remaining body into N tracks.
 *
 * Every dip in the envelope is a candidate cut and gets a score: how far
 * the level drops compared to the music on both sides, and how long it
 * stays down at the noise floor.  A dynamic program then picks the N-1
 * candidates with the highest total score.  Tracks shorter than
 * --min-length cost a penalty, so a short track only appears when the
 * pauses around it are obvious enough to pay for it.
 *
 * Pass 2 copies the byte ranges out.  Nothing is discarded: the output
 * files concatenated are identical to the input (unless --no-edges is
 * used, which skips writing the lead-in and run-out files).
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#define RATE          44100
#define FRAME_BYTES   4                 /* 16 bit stereo */
#define BLOCK         882               /* sample frames per envelope block */
#define BPS           (RATE / BLOCK)    /* envelope blocks per second: 50 */
#define SEC(s)        ((int) ((s) * BPS))

/* Tuning.  All times in seconds, all levels in dB. */
#define SMOOTH          0.3     /* envelope smoothing window */
#define MERGE_GAP       0.25    /* bridge shorter gaps between loud bits */
#define BURST_MAX       3.0     /* needle noise is shorter than this ... */
#define BURST_GAP       1.0     /* ... and separated from music by this */
#define LEADIN_MAX      60.0    /* look for needle-drop noise this far in */
#define RUNOUT_MAX      120.0   /* look for run-out noise this far from end */
#define EDGE_SILENCE    5.0     /* split off pure silence if this long */
#define CAND_RADIUS     2.0     /* a candidate is the lowest point +- this */
#define CONTEXT         10.0    /* music level measured this far each side */
#define HARD_MIN        10.0    /* never make a track shorter than this */
#define SILENCE_BONUS   8.0     /* score per second of real silence ... */
#define SILENCE_CAP     5.0     /* ... up to this many seconds */
#define DEPTH_CAP       40.0
#define SHORT_PENALTY   60.0    /* cost of a (nearly) zero-length track */
#define WEAK_SCORE      12.0    /* below this, flag the cut as a guess */

static int ntracks = 0;
static double minlen = 120;
static char *output = "output";
static double user_threshold = -1;
static int dry_run = 0;
static int verbose = 0;
static int no_edges = 0;

static int nblocks;             /* length of the file in envelope blocks */
static double *psum;            /* prefix sums of block power */
static float *env;              /* smoothed envelope in dB */

struct region { int start, end; };

static void die (const char *msg) {
  perror (msg);
  exit (1);
}

static void *xrealloc (void *p, size_t n) {
  if (! (p = realloc (p, n))) {
    fprintf (stderr, "vsplit: out of memory\n");
    exit (1);
  }
  return p;
}

static int clampi (int v, int lo, int hi) {
  return v < lo? lo: v > hi? hi: v;
}

static double clampd (double v, double lo, double hi) {
  return v < lo? lo: v > hi? hi: v;
}

static char *tstr (int block) {
  static char bufs[4][16];
  static int n = 0;
  char *s = bufs[n++ & 3];
  int tenths = (block * 10 + BPS / 2) / BPS;
  snprintf (s, 16, "%02d:%02d.%d", tenths / 600, tenths / 10 % 60,
	    tenths % 10);
  return s;
}

/* Mean power of blocks [a, b) in dB. */
static double level (int a, int b) {
  a = clampi (a, 0, nblocks);
  b = clampi (b, 0, nblocks);
  if (b <= a)
    return -20;
  return 10 * log10 ((psum[b] - psum[a]) / (b - a) + 0.01);
}

static int cmp_float (const void *a, const void *b) {
  float x = *(const float *) a, y = *(const float *) b;
  return (x > y) - (x < y);
}

/* The q'th quantile of env[a, b). */
static double quantile (int a, int b, double q) {
  int n = b - a;
  float *tmp = xrealloc (NULL, n * sizeof (float));
  double r;
  memcpy (tmp, env + a, n * sizeof (float));
  qsort (tmp, n, sizeof (float), cmp_float);
  r = tmp[(int) (q * (n - 1))];
  free (tmp);
  return r;
}

/*
 * Pass 1: reduce the file to one power value per block.  The power is that
 * of the first difference of the mono signal -- a crude high-pass filter
 * that keeps turntable rumble and DC offsets from registering as sound.
 */
static void analyze (const char *file) {
  FILE *f;
  int16_t buf[BLOCK * 2];
  double *pw = NULL;
  int alloc = 0, n, i, half = SEC (SMOOTH) / 2;
  int prev = 0;

  if (! (f = fopen (file, "rb")))
    die (file);

  while ((n = fread (buf, FRAME_BYTES, BLOCK, f)) > 0) {
    double sum = 0;
    for (i = 0; i < n; i++) {
      int mono = (buf[i * 2] + buf[i * 2 + 1]) / 2;
      int d = mono - prev;
      sum += (double) d * d;
      prev = mono;
    }
    if (nblocks >= alloc) {
      alloc = alloc? alloc * 2: 1 << 16;
      pw = xrealloc (pw, alloc * sizeof (double));
    }
    pw[nblocks++] = sum / n;
  }
  if (ferror (f))
    die (file);
  fclose (f);

  psum = xrealloc (NULL, (nblocks + 1) * sizeof (double));
  psum[0] = 0;
  for (i = 0; i < nblocks; i++)
    psum[i + 1] = psum[i] + pw[i];
  free (pw);

  env = xrealloc (NULL, (nblocks + 1) * sizeof (float));
  for (i = 0; i < nblocks; i++)
    env[i] = level (i - half, i + half + 1);
}

/* Find the longest stretch of [a, b) that's down at the noise floor.
   Returns its length, and its start in *start. */
static int quiet_run (int a, int b, double quiet, int *start) {
  int j, run = 0, best = 0;

  for (j = a; j <= b; j++) {
    if (j < b && env[j] <= quiet)
      run++;
    else {
      if (run > best) {
	best = run;
	if (start)
	  *start = j - run;
      }
      run = 0;
    }
  }
  return best;
}

/*
 * Find the silence in the dip [a, b) where one track has ended and the
 * next hasn't begun.  That's the first stretch at the noise floor that
 * lasts a second or more; anything audible after it -- however faint, and
 * even if there's more silence after that -- is taken to be the start of
 * the next track.  Ticks, and a fade-out hovering just above the floor,
 * don't count as audible, and don't break up the silence.
 */
static int first_silence (int a, int b, double quiet, int *start, int *end) {
  int j = a, s = -1, e = -1;

  while (j < b) {
    if (env[j] <= quiet) {
      if (s < 0)
	s = j;
      while (j < b && env[j] <= quiet)
	j++;
      e = j;
    } else {
      int from = j;
      double peak = env[j];
      for (; j < b && env[j] > quiet; j++)
	if (env[j] > peak)
	  peak = env[j];
      if (s >= 0 && j - from >= SEC (1.5) && peak > quiet + 2) {
	if (e - s >= SEC (1))
	  break;
	s = -1;                 /* too brief to be the gap; keep looking */
      }
    }
  }
  if (s < 0 || e - s < SEC (0.5))
    return 0;
  *start = s;
  *end = e;
  return 1;
}

/* Find the stretches louder than thr, bridging very short gaps. */
static struct region *find_regions (double thr, int *count) {
  struct region *reg = NULL;
  int n = 0, alloc = 0, i = 0;

  while (i < nblocks) {
    int start;
    while (i < nblocks && env[i] <= thr)
      i++;
    if (i >= nblocks)
      break;
    start = i;
    while (i < nblocks && env[i] > thr)
      i++;
    if (n && start - reg[n - 1].end < SEC (MERGE_GAP))
      reg[n - 1].end = i;
    else {
      if (n >= alloc) {
	alloc = alloc? alloc * 2: 256;
	reg = xrealloc (reg, alloc * sizeof (struct region));
      }
      reg[n].start = start;
      reg[n++].end = i;
    }
  }
  *count = n;
  return reg;
}

static double penalty (int len) {
  double m = minlen * BPS;
  return len >= m? 0: SHORT_PENALTY * (1 - len / m);
}

/*
 * Choose ncuts cut points in the body [bs, be).  Returns the number of
 * cuts found (ncuts, or 0 on failure), cut positions in cuts[], scores in
 * cscore[] and the length of the silence at the cut (0 if none) in cgap[].
 * "quiet" is the level below which we're hearing nothing but the groove.
 */
static int choose_cuts (int bs, int be, double thr, double quiet, int ncuts,
			int *cuts, double *cscore, int *cgap) {
  int radius = SEC (CAND_RADIUS), ctx = SEC (CONTEXT);
  int *pos, *gap, *prev;
  double *score, *best;
  int nc = 0, alloc = 0, i, j, k, jp, hardmin, attempt, found = 0;

  if (ncuts == 0)
    return 0;

  /* Node 0 is the start of the body; candidates are nodes 1..nc. */
  alloc = 1024;
  pos = xrealloc (NULL, alloc * sizeof (int));
  gap = xrealloc (NULL, alloc * sizeof (int));
  score = xrealloc (NULL, alloc * sizeof (double));
  pos[0] = bs;

  for (i = bs + 1; i < be - 1; i++) {
    int a, b, cut, qa = 0, qb = 0, qlen, is_min = 1;
    double left, right, depth, s;

    /* A candidate is the lowest point in its neighbourhood. */
    for (j = (i - radius < bs? bs: i - radius); j < i && is_min; j++)
      if (env[j] <= env[i])
	is_min = 0;
    for (j = i + 1; j <= i + radius && j < be && is_min; j++)
      if (env[j] < env[i])
	is_min = 0;
    if (! is_min)
      continue;

    qlen = 0;
    if (env[i] < thr) {
      /* A dip below the threshold: find its extent. */
      for (a = i; a > bs && env[a - 1] < thr; a--)
	;
      for (b = i + 1; b < be && env[b] < thr; b++)
	;
      /* The dip may include a fade-out and a quiet intro, and we want
	 to cut in the silence between them. */
      qlen = quiet_run (a, b, quiet, NULL);
      if (first_silence (a, b, quiet, &qa, &qb)) {
	/* Cut a second before the next track starts to emerge from the
	   noise (a slow fade-in is in progress well before it's "quiet"
	   no longer: look back for the last two seconds of nothing at
	   all), but stay clear of the first quarter of the silence,
	   where the previous track may still be dying away. */
	int mid = qa + (qb - qa) / 4, run = 0;
	cut = qb;
	for (j = qb - 1; j >= mid; j--) {
	  run = env[j] <= quiet - 2? run + 1: 0;
	  if (run >= SEC (2)) {
	    cut = j + run;
	    break;
	  }
	}
	cut = cut - BPS > mid? cut - BPS: mid;
      } else {
	qlen = 0;
	cut = b - (b - a > 2 * BPS? BPS: (b - a) / 2);
      }
    } else {
      a = i - BPS;
      b = i + BPS;
      cut = i;
    }

    /* How far below the music on either side is this? */
    left = a - bs >= BPS?
      quantile (a - ctx < bs? bs: a - ctx, a, 0.5): -1000;
    right = be - b >= BPS?
      quantile (b, b + ctx > be? be: b + ctx, 0.5): -1000;
    if (left == -1000)
      left = right;
    if (right == -1000)
      right = left;
    depth = clampd ((left < right? left: right) - env[i], 0, DEPTH_CAP);

    /* A dip is worth its depth, plus a little if the music is at a
       different level afterwards, plus a lot for every second it spends
       right down at the noise floor.  That last part is what tells a gap
       between tracks from a pause within one: music that stops for a
       moment usually leaves something ringing. */
    s = depth + 0.5 * clampd (fabs (left - right), 0, 10)
      + SILENCE_BONUS * clampd ((double) qlen / BPS, 0, SILENCE_CAP);

    if (cut <= bs || cut >= be)
      continue;
    if (cut <= pos[nc]) {
      /* Same pause as the previous candidate. */
      if (nc && s > score[nc])
	score[nc] = s;
      continue;
    }
    if (++nc >= alloc) {
      alloc *= 2;
      pos = xrealloc (pos, alloc * sizeof (int));
      gap = xrealloc (gap, alloc * sizeof (int));
      score = xrealloc (score, alloc * sizeof (double));
    }
    pos[nc] = cut;
    gap[nc] = qlen;
    score[nc] = s;
    if (verbose)
      fprintf (stderr, "  candidate %s  score %5.1f  %s\n", tstr (cut), s,
	       gap[nc]? "silence": "");
  }

  /* best[k][j]: best total with k cuts made, the latest at node j. */
#define NONE -1e30
#define B(k, j) best[(k) * (nc + 1) + (j)]
#define P(k, j) prev[(k) * (nc + 1) + (j)]
  best = xrealloc (NULL, (ncuts + 1) * (nc + 1) * sizeof (double));
  prev = xrealloc (NULL, (ncuts + 1) * (nc + 1) * sizeof (int));

  /* Try with the hard minimum track length first; if the record can't be
     cut that way, try again without it. */
  hardmin = SEC (HARD_MIN < minlen? HARD_MIN: minlen);
  for (attempt = 0; attempt < 2 && ! found; attempt++, hardmin = 1) {
    double top = NONE;
    int end = 0;

    for (k = 0; k <= ncuts; k++)
      for (j = 0; j <= nc; j++)
	B (k, j) = NONE;
    B (0, 0) = 0;

    for (k = 1; k <= ncuts; k++)
      for (j = k; j <= nc; j++)
	for (jp = k - 1; jp < j; jp++) {
	  int len = pos[j] - pos[jp];
	  double v;
	  if (B (k - 1, jp) == NONE || len < hardmin)
	    continue;
	  v = B (k - 1, jp) + score[j] - penalty (len);
	  if (v > B (k, j)) {
	    B (k, j) = v;
	    P (k, j) = jp;
	  }
	}

    for (j = ncuts; j <= nc; j++) {
      int len = be - pos[j];
      if (B (ncuts, j) == NONE || len < hardmin)
	continue;
      if (B (ncuts, j) - penalty (len) > top) {
	top = B (ncuts, j) - penalty (len);
	end = j;
      }
    }

    if (top != NONE) {
      for (k = ncuts, j = end; k > 0; j = P (k, j), k--) {
	cuts[k - 1] = pos[j];
	cscore[k - 1] = score[j];
	cgap[k - 1] = gap[j];
      }
      found = ncuts;
    }
  }

  free (pos); free (gap); free (score); free (best); free (prev);
  return found;
}

/* Pass 2: copy bytes [from, to) of the input to a new file. */
static void copy_range (int in, const char *name, off_t from, off_t to) {
  static char buf[1 << 16];
  int out;

  if ((out = open (name, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0)
    die (name);
  if (lseek (in, from, SEEK_SET) < 0)
    die ("vsplit seek");
  while (from < to) {
    ssize_t want = to - from > (off_t) sizeof buf? (ssize_t) sizeof buf:
      (ssize_t) (to - from);
    ssize_t got = read (in, buf, want), done = 0, w;
    if (got < 0)
      die ("vsplit read");
    if (got == 0)
      break;
    while (done < got) {
      if ((w = write (out, buf + done, got - done)) < 0)
	die (name);
      done += w;
    }
    from += got;
  }
  if (close (out) < 0)
    die (name);
}

static void emit (int in, off_t size, const char *suffix, int from, int to,
		  const char *note, int write_it) {
  char name[4096];
  off_t bfrom = (off_t) from * BLOCK * FRAME_BYTES;
  off_t bto = to >= nblocks? size: (off_t) to * BLOCK * FRAME_BYTES;

  if (write_it)
    snprintf (name, sizeof name, "%s-%s.raw", output, suffix);
  else
    snprintf (name, sizeof name, "[%s, not written]", suffix);
  printf ("%s - %s  %7s  %s%s%s\n", tstr (from), tstr (to),
	  tstr (to - from), name, *note? "  ": "", note);
  if (! dry_run && write_it)
    copy_range (in, name, bfrom, bto);
}

static void split (const char *file) {
  struct region *reg;
  struct stat st;
  int nreg, first, last, bs, be, lo, hi, i, in, n;
  int *cuts, *cgap;
  double *cscore;
  double ref, floor_db, global_floor, thr, quiet;
  char suffix[32], note[128];

  analyze (file);
  if (nblocks < SEC (1)) {
    fprintf (stderr, "vsplit: %s is too short\n", file);
    exit (1);
  }

  /* Reference levels.  "ref" is loud music.  The noise floor is the
     quietest second in the middle of the side -- with luck that's a pause
     between tracks, i.e. bare groove noise.  Using the middle avoids being
     fooled by the dead silence before the needle drops or after it lifts. */
  ref = quantile (0, nblocks, 0.9);
  floor_db = global_floor = 1000;
  lo = nblocks / 10;
  hi = nblocks - nblocks / 10 - BPS;
  for (i = 0; i + BPS <= nblocks; i++) {
    double l = level (i, i + BPS);
    if (l < global_floor)
      global_floor = l;
    if (i >= lo && i <= hi && l < floor_db)
      floor_db = l;
  }
  /* If the middle never gets properly quiet (the tracks run together),
     the lead-in or run-out may still tell us what the floor is. */
  if (floor_db > ref - 30)
    floor_db = global_floor > ref - 30? global_floor: ref - 30;
  /* And if there's no silence anywhere, guess. */
  if (floor_db > ref - 20)
    floor_db = ref - 30;
  if (floor_db < ref - 60)
    floor_db = ref - 60;
  quiet = floor_db + 3;
  if (user_threshold >= 0)
    thr = ref - user_threshold;
  else
    thr = floor_db + ((ref - floor_db) / 3 > 6? (ref - floor_db) / 3: 6);

  fprintf (stderr, "%s: %s long; music %.1fdB, floor %.1fdB, "
	   "silence threshold %.1fdB (-t %.0f)\n",
	   file, tstr (nblocks), ref, floor_db, thr, ref - thr);

  reg = find_regions (thr, &nreg);
  if (! nreg) {
    fprintf (stderr, "vsplit: nothing but silence in %s\n", file);
    exit (1);
  }

  /* Lead-in: short bursts of noise near the start, each followed by
     silence, are the needle landing rather than music.  The silence has
     to be right down at the noise floor, or an intro of separate notes
     with the sound dying away between them would qualify.  A burst may
     be several thumps in quick succession. */
  first = 0;
  for (;;) {
    for (i = first; i < nreg - 1 &&
	   reg[i + 1].start - reg[i].end < SEC (BURST_GAP); i++)
      ;
    if (i >= nreg - 1 ||
	reg[first].start >= SEC (LEADIN_MAX) ||
	reg[i].end - reg[first].start >= SEC (BURST_MAX) ||
	quiet_run (reg[i].end, reg[i + 1].start, quiet, NULL)
	< SEC (BURST_GAP))
      break;
    first = i + 1;
  }

  /* Run-out: the same thing from the other end. */
  last = nreg - 1;
  for (;;) {
    for (i = last; i > first &&
	   reg[i].start - reg[i - 1].end < SEC (BURST_GAP); i--)
      ;
    if (i <= first ||
	reg[i].start <= nblocks - SEC (RUNOUT_MAX) ||
	reg[last].end - reg[i].start >= SEC (BURST_MAX) ||
	quiet_run (reg[i - 1].end, reg[i].start, quiet, NULL)
	< SEC (BURST_GAP))
      break;
    last = i - 1;
  }

  /* The music starts/ends where the envelope crosses the threshold, but
     move outwards to catch quiet intros and fade-outs, stopping at the
     noise floor or at the needle noise. */
  bs = reg[first].start;
  lo = first? reg[first - 1].end: 0;
  if (lo < bs - SEC (3))
    lo = bs - SEC (3);
  while (bs > lo && env[bs - 1] > quiet)
    bs--;
  bs = clampi (bs - SEC (0.5), lo, bs);

  be = reg[last].end;
  hi = last < nreg - 1? reg[last + 1].start: nblocks;
  if (hi > be + SEC (10))
    hi = be + SEC (10);
  /* (Be more persistent at the end: a long fade-out sinks into the noise
     very gradually, and a few seconds of extra groove noise on the last
     track does no harm.) */
  while (be < hi && env[be] > floor_db + 1.5)
    be++;
  be = clampi (be + SEC (2), be, last < nreg - 1? reg[last + 1].start: nblocks);

  /* Without any needle noise, only split off the edges if there's a
     worthwhile amount of silence there. */
  if (! first && bs < SEC (EDGE_SILENCE))
    bs = 0;
  if (last == nreg - 1 && nblocks - be < SEC (EDGE_SILENCE))
    be = nblocks;

  cuts = xrealloc (NULL, (ntracks + 1) * sizeof (int));
  cgap = xrealloc (NULL, (ntracks + 1) * sizeof (int));
  cscore = xrealloc (NULL, (ntracks + 1) * sizeof (double));
  n = choose_cuts (bs, be, thr, quiet, ntracks - 1, cuts, cscore,
		   cgap);
  if (n != ntracks - 1) {
    fprintf (stderr, "vsplit: can't find %d places to split %s of music\n",
	     ntracks - 1, tstr (be - bs));
    exit (1);
  }
  cuts[ntracks - 1] = be;

  if ((in = open (file, O_RDONLY)) < 0 || fstat (in, &st) < 0)
    die (file);

  if (bs > 0)
    emit (in, st.st_size, "leadin", 0, bs,
	  first? "(needle drop)": "(silence)", ! no_edges);
  for (i = 0; i < ntracks; i++) {
    int from = i? cuts[i - 1]: bs;
    *note = 0;
    if (i && cgap[i - 1])
      snprintf (note, sizeof note, "(after %.1fs of silence, score %.0f)",
		(double) cgap[i - 1] / BPS, cscore[i - 1]);
    else if (i)
      snprintf (note, sizeof note, "(no silence, score %.0f%s)", cscore[i - 1],
		cscore[i - 1] < WEAK_SCORE? " -- A GUESS": "");
    snprintf (suffix, sizeof suffix, "%02d", i + 1);
    emit (in, st.st_size, suffix, from, cuts[i], note, 1);
  }
  if (be < nblocks)
    emit (in, st.st_size, "runout", be, nblocks,
	  last < nreg - 1? "(run-out noise)": "(silence)", ! no_edges);
  close (in);
}

static void usage (FILE *f) {
  fprintf (f, "\
Usage: vsplit --tracks <n> [--min-length <seconds>] [--output <prefix>]\n\
              [--threshold <dB>] [--no-edges] [--dry-run] [--verbose] <file>\n\
\n\
<file> is one record side as raw 44.1kHz 16-bit stereo.\n\
\n\
--tracks      the number of tracks on the side (required)\n\
--min-length  tracks shorter than this are only split off if the pauses\n\
              around them are unmistakable (default 120)\n\
--output      output files are <prefix>-01.raw etc., plus\n\
              <prefix>-leadin.raw and <prefix>-runout.raw (default \"output\")\n\
--threshold   count as silence anything this many dB below the music;\n\
              the default is worked out from the file and printed\n\
--no-edges    don't write the lead-in and run-out files; the tracks are\n\
              the same, the audio outside them is just dropped\n\
--dry-run     show where the splits would be, but write nothing\n\
--verbose     list every candidate split point and its score\n\
\n\
Example: vsplit -n 5 -o side-a side-a.raw\n");
}

int main (int argc, char **argv) {
  static struct option long_options[] = {
    {"help", no_argument, 0, 'h'},
    {"tracks", required_argument, 0, 'n'},
    {"min-length", required_argument, 0, 'm'},
    {"output", required_argument, 0, 'o'},
    {"threshold", required_argument, 0, 't'},
    {"no-edges", no_argument, 0, 'x'},
    {"dry-run", no_argument, 0, 'd'},
    {"verbose", no_argument, 0, 'v'},
    {0, 0, 0, 0}
  };
  int c;

  while ((c = getopt_long (argc, argv, "hn:m:o:t:xdv", long_options, 0))
	 != -1) {
    switch (c) {
    case 'h':
      usage (stdout);
      return 0;
    case 'n':
      ntracks = atoi (optarg);
      break;
    case 'm':
      minlen = atof (optarg);
      break;
    case 'o':
      output = optarg;
      break;
    case 't':
      user_threshold = atof (optarg);
      break;
    case 'x':
      no_edges = 1;
      break;
    case 'd':
      dry_run = 1;
      break;
    case 'v':
      verbose = 1;
      break;
    default:
      usage (stderr);
      return 1;
    }
  }

  if (ntracks < 1 || optind != argc - 1) {
    usage (stderr);
    return 1;
  }

  split (argv[optind]);
  return 0;
}
