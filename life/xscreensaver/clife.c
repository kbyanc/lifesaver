/*
 * Copyright (c) 2003,2007 Kelly Yancey (kbyanc@posi.net)
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation.  No representations are made about the suitability of this
 * software for any purpose.  It is provided "as is" without express or 
 * implied warranty.
 *
 * $kbyanc: life/xscreensaver/clife.c,v 1.16 2007/04/18 02:22:13 kbyanc Exp $
 */

/* Undefine the following before testing any code changes! */
#define NDEBUG

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <assert.h>
#include <dirent.h>
#include <limits.h>
#include "screenhack.h"

#ifdef HAVE_DOUBLE_BUFFER_EXTENSION
# include "xdbe.h"
#endif /* HAVE_DOUBLE_BUFFER_EXTENSION */


/*
 * Additional debugging aids:
 *	LIFE_SHOWGRID	   - Define to show cell cluster boundaries.
 *	LIFE_PRINTSTATS	   - Define to have cell/cluster stats printed.
 *	LIFE_PRINTPATTERNS - Define to print which patterns were loaded.
 */
#undef LIFE_SHOWGRID
#undef LIFE_PRINTSTATS
#undef LIFE_PRINTPATTERNS


/*
 * Data structures for representing Life patterns.
 * We will attempt to load any Life 1.05 format pattern files found in the
 * path specified via the -patterns command-line parameter or the patternPath
 * resource.  To keep things interesting for people who do not have a pattern
 * collection to pull from, we include 3 built-in patterns.  Please don't add
 * more.  If you want more patterns, store them in files and put the directory
 * to find those files in your patternPath.
 *
 * The number of cells for a single pattern is limited to a maximum of
 * PATTERN_MAXCOORDS cells.
 */
#define	PATTERN_MAXCOORDS	256
struct coords {
	int x, y;
};
static struct coords builtin_pattern_coords[] = {
#define	BUILTIN_PATTERN_GLIDER	(builtin_pattern_coords + 0)
	{ 0, 0 }, { 1, 0 }, { 2, 0 },
	{ 0, 1 },
		  { 1, 2 },

#define	BUILTIN_PATTERN_BHEPT	(builtin_pattern_coords + 5)
		  { 1, 0 },
	{ 0, 1 }, { 1, 1 }, { 2, 1 },
	{ 0, 2 },	    { 2, 2 }, { 3, 2 },

#define	BUILTIN_PATTERN_RABBITS	(builtin_pattern_coords + 12)
	{ 0, 0 }, 				{ 4, 0 }, { 5, 0 }, { 6, 0 },
	{ 0, 1 }, { 1, 1 }, { 2, 1 },			  { 5, 1 },
		  { 1, 2 },

	/* Offset 21: End-of-List */
};

struct pattern {
	unsigned int width, height;
	unsigned int numcoords;
	struct coords *coords;
};

#define	NUMPATTERNS		16
#define	NUMPATTERNSBUILTIN	3	/* Please don't add more. */
static const struct pattern builtin_patterns[NUMPATTERNSBUILTIN] = {
	{  3,  3,  5, BUILTIN_PATTERN_GLIDER },
	{  4,  3,  7, BUILTIN_PATTERN_BHEPT },
	{  7,  3,  9, BUILTIN_PATTERN_RABBITS }
};


/*
 * The following limits apply to dormant clusters.
 *	LIMIT_DRAW	- Clusters dormant for more than this many iterations
 *			  will not be drawn.  Must be at least 1.
 *	LIMIT_UPDATE	- Clusters dormant for more than this many iterations
 *			  will not be updated; useful for skipping static
 *			  debris.  Must be at least 1 more than LIMIT_DRAW.
 *	LIMIT_KEEPEMPTY	- Clusters with no cells are kept for this many cycles
 *			  before deleting.  Should be greater than LIMIT_UPDATE.
 */
#define	LIMIT_DRAW	1
#define	LIMIT_UPDATE	LIMIT_DRAW + 1
#define	LIMIT_KEEPEMPTY	16


typedef	unsigned char cell;
#define	CELL_DEAD	0
#define	CELL_MINALIVE	1
#define	CELL_MAXCOLORS	85	/* Must be less than or equal to 1/3 of the
				 * maximum value representable by cell's type.
				 * See note in life_display_init().
				 */

/*
 * The directions must be ordered such that direction (X) and direction
 * (NUMDIRECTIONS - X - 1) are opposites.  The following ordering is optimal
 * because it follows this rule and is ordered by increasing Y offset followed
 * by increasing X offset.
 */
#define NUMDIRECTIONS	8
enum direction {
	NORTHWEST = 0,
	NORTH,
	NORTHEAST,
	WEST,
	EAST,
	SOUTHWEST,
	SOUTH,
	SOUTHEAST
};


/*
 * Since the cell universe is often sparse, it is divided up into clusters
 * CLUSTERSIZE cells per side.  This allows regions known to be empty to
 * be skipped in the simulation loop.
 *
 * To prevent the frequent creation/deletion of nearly-empty clusters (i.e.
 * when a blinker crosses a cluster boundary), clusters are not immediately
 * freed when they become empty, but instead only after MAXDORMANTAGE
 * iterations empty.  Similarly, when a neighboring cluster has cells on the
 * adjacent edge, it will clear the dormant counter as an indication that the
 * cluster should not be deleted as it needs to check for spillover.
 *
 * The dormant counter is also used to avoid processing clusters which contain
 * static debris, as is common in life.
 */
#define	CLUSTERSIZE	8	/* Number of cells per cluster; power-of-2. */
struct cell_cluster {
	short			 numcells;
	unsigned char		 dormant;	/* Iterations unchanged. */
	unsigned char		 reserved;
	int			 clusterX, clusterY;
	struct cell_cluster	*neighbor[NUMDIRECTIONS];
	cell			 oldcell[CLUSTERSIZE][CLUSTERSIZE];
	cell			 cell[CLUSTERSIZE][CLUSTERSIZE];
	u_int8_t		 cellage[CLUSTERSIZE][CLUSTERSIZE];
};


struct state {
	/*
	 * Display parameters.
	 */
	int	 delay;
	int	 numcolors;
	int	 colorwrap;
	int	 double_buffer;
	int	 DBEclear;	/* Use DBE to clear buffer. */

	/* Internal display variables. */
	XWindowAttributes xgwa;
	XColor	*colors;
	XColor	*trailcolors;
	GC	 gc_erase;
	GC	 gc_draw;
	int	 display_offsetX;
	int	 display_offsetY;
	Pixmap	 buf;		/* Current work buffer. */
	Pixmap	 pixmap;	/* Backing pixmap, if any. */

#ifdef HAVE_DOUBLE_BUFFER_EXTENSION
	XdbeBackBuffer backbuf;
	XdbeSwapInfo swapinfo;
#endif

	/*
	 * Simulation state.
	 */
	struct cell_cluster **clustertable;

	int	 numclusters;	/* Number of clusters allocated. */
	int	 numcells;	/* Number of cells in those clusters. */
	int	 maxclusters;
	int	 maxcells;
	int	 cluster_numX;
	int	 cluster_numY;
	int	 cell_numX;
	int	 cell_numY;
	int	 cellsize;	/* Size of cells in pixels. */
	int	 celldrawsize;	/* Actual number of pixels drawn per cell. */
	int	 cellmaxage;	/* Cells die when they reach this age. */
	unsigned int iteration;

	/*
	 * Pattern data.
	 */
	struct pattern patterns[NUMPATTERNS];
};


static struct cell_cluster *life_cluster_new(struct state *st,
					     int clusterX, int clusterY);
static void	 life_cluster_delete(struct state *st,
				     struct cell_cluster *cluster);
static void	 life_cluster_update(struct state *st,
				     struct cell_cluster *cluster);
static void	 life_cluster_draw(const struct state * const st,
				   Display *dpy, Window window,
				   const struct cell_cluster * const cluster,
				   int xoffset, int yoffset);
static void	 life_cluster_wakeneighbor(struct state *st,
					   const struct cell_cluster *cluster,
					   int xoffset, int yoffset);
static void	 life_cell_set(struct state *st, int x, int y, int color);

static void	 life_pattern_init(struct state *st, Display *dpy);
static void	 life_pattern_free(struct state *st);
static void	 life_pattern_draw(struct state *st);
static int	 life_pattern_read(const struct state * const st,
				   const char *filename,
				   struct pattern *pattern);

static void	 life_state_init(struct state *st, Display *dpy);
static void	 life_state_free(struct state *st);
static void	 life_state_update(struct state *st);

static void	 life_display_init(struct state *st, Display *dpy,
				   Window window);
static void	 life_display_free(struct state *st, Display *dpy);
static void	 life_display_update(struct state *st,
				     Display *dpy, Window window);



void
life_state_init(struct state *st, Display *dpy)
{

	st->cellmaxage = get_integer_resource(dpy, "maxAge", "Integer");
	if (st->cellmaxage < 1)
		st->cellmaxage = 0;
	else if (st->cellmaxage > UCHAR_MAX)
		st->cellmaxage = UCHAR_MAX;

	st->cellsize = get_integer_resource(dpy, "cellSize", "Integer");
	if (st->cellsize < 1)
		st->cellsize = 1;

	for (;;) {
		st->cell_numX = st->xgwa.width / st->cellsize;
		st->cell_numY = st->xgwa.height / st->cellsize;

		st->cluster_numX = st->cell_numX / CLUSTERSIZE;
		st->cluster_numY = st->cell_numY / CLUSTERSIZE;

		if (st->cluster_numX >= 1 && st->cluster_numY >= 1)
			break;

		/*
		 * The cells are too big to fit at least 1 cluster on the
		 * display.  Reduce the cell size until a cluster fits.
		 */
		st->cellsize--;
		if (st->cellsize < 0)
			exit(1);
	}

	/*
	 * Adjust size of drawn cell to account for any cell border.
	 */
	st->celldrawsize = st->cellsize;
	if (st->celldrawsize > 1 &&
	    get_boolean_resource(dpy, "cellBorder", "Boolean"))
		st->celldrawsize--;

	/*
	 * Recompute the number of cells as a multiple of the number of
	 * clusters.
	 */
	st->cell_numX = st->cluster_numX * CLUSTERSIZE;
	st->cell_numY = st->cluster_numY * CLUSTERSIZE;

	/* Center the cell display. */
	st->display_offsetX = (st->xgwa.width -
			       (st->cell_numX * st->cellsize)) / 2;
	st->display_offsetY = (st->xgwa.height -
			       (st->cell_numY * st->cellsize)) / 2;

	/*
	 * Allocate the cluster lookup table.  Initialize all pointers to
	 * NULL to indicate an empty universe.
	 */
	st->maxcells = st->cell_numX * st->cell_numY;
	st->maxclusters = st->cluster_numX * st->cluster_numY;
	st->clustertable = calloc(st->maxclusters,
				  sizeof(*st->clustertable));
	if (st->clustertable == NULL)
		exit(1);
	st->numcells = 0;
	st->numclusters = 0;
	st->iteration = 0;
}


void
life_state_free(struct state *st)
{

	free(st->clustertable);
}


void
life_state_update(struct state *st)
{
	struct cell_cluster **clustertable = st->clustertable;
	struct cell_cluster *cluster;
	int clusteridx;
	int numactive;

	numactive = 0;
	for (clusteridx = 0; clusteridx < st->maxclusters; clusteridx++) {
		cluster = clustertable[clusteridx];
		if (cluster == NULL)
			continue;
		if (cluster->dormant < LIMIT_UPDATE) {
			life_cluster_update(st, cluster);
			numactive++;
			continue;
		}
		cluster->dormant++;
		if (cluster->dormant > LIMIT_KEEPEMPTY)
			life_cluster_delete(st, cluster);
	}

	/* Try to keep the display at least 6.25% full. */
	if (st->iteration % 256 == 0 ||
	    st->numclusters * 16 < st->maxclusters)
		life_pattern_draw(st);

#ifdef LIFE_PRINTSTATS
	fprintf(stderr,
		"%03d/%03d clusters (%03d active: %02d%%); %05d/%05d cells\n",
		st->numclusters, st->maxclusters, numactive,
		numactive * 100 / st->maxclusters,
		st->numcells, st->maxcells);
#endif

	st->iteration++;
}


static __inline
void
life_cluster_wakeneighbor(struct state *st, const struct cell_cluster *cluster,
			  int xoffset, int yoffset)
{
	life_cluster_new(st, cluster->clusterX + xoffset,
			 cluster->clusterY + yoffset);
}


void
life_cluster_update(struct state *st, struct cell_cluster *cluster)
{
	static cell state[CLUSTERSIZE + 2][CLUSTERSIZE + 2];
	struct cell_cluster *neighbor;
	int cellX, cellY;
	int x, y, count;
	int cellval, color;
	unsigned int changemapX, changemapY;
	int deaths, births;
	int sum;
	int idx;

	changemapX = changemapY = 0;
	deaths = births = 0;
	memset(state, CELL_DEAD, sizeof(state));

	/*
	 * First populate the simulation state buffer.  The edges come from
	 * neighboring clusters.
	 */
	if ((neighbor = cluster->neighbor[NORTHWEST]) != NULL)
		state[0][0] = neighbor->oldcell[CLUSTERSIZE-1][CLUSTERSIZE-1];

	if ((neighbor = cluster->neighbor[NORTHEAST]) != NULL)
		state[0][CLUSTERSIZE+1] = neighbor->oldcell[CLUSTERSIZE-1][0];

	if ((neighbor = cluster->neighbor[SOUTHWEST]) != NULL)
		state[CLUSTERSIZE+1][0] = neighbor->oldcell[0][CLUSTERSIZE-1];

	if ((neighbor = cluster->neighbor[SOUTHEAST]) != NULL)
		state[CLUSTERSIZE+1][CLUSTERSIZE+1] = neighbor->oldcell[0][0];

	if ((neighbor = cluster->neighbor[NORTH]) != NULL) {
		memcpy(&state[0][1], &neighbor->oldcell[CLUSTERSIZE-1][0],
		       CLUSTERSIZE * sizeof(cell));
	}

	if ((neighbor = cluster->neighbor[SOUTH]) != NULL) {
		memcpy(&state[CLUSTERSIZE + 1][1], &neighbor->oldcell[0][0],
		       CLUSTERSIZE * sizeof(cell));
	}

	if ((neighbor = cluster->neighbor[WEST]) != NULL) {
		for (idx = 0; idx < CLUSTERSIZE; idx++)
			state[idx+1][0] = neighbor->oldcell[idx][CLUSTERSIZE-1];
	}

	if ((neighbor = cluster->neighbor[EAST]) != NULL) {
		for (idx = 0; idx < CLUSTERSIZE; idx++)
			state[idx+1][CLUSTERSIZE+1] = neighbor->oldcell[idx][0];
	}

	/* Copy the middle from the current cluster's old cell state. */
	for (idx = 0; idx < CLUSTERSIZE; idx++) {
		memcpy(&state[idx+1][1], &cluster->oldcell[idx][0],
		       CLUSTERSIZE * sizeof(cell));
	}

	/*
	 * Now, we can calculate the current state for this cluster.
	 */
	for (cellY = 0; cellY < CLUSTERSIZE; cellY++) {
		for (cellX = 0; cellX < CLUSTERSIZE; cellX++) {

			cellval = state[cellY + 1][cellX + 1];
			count = sum = 0;

			/*
			 * Examine each neighbor; offset by 1 due to padding in
			 * local state buffer.
			 */
			for (y = cellY; y <= cellY + 2; y++) {
				for (x = cellX; x <= cellX + 2; x++) {
					if (state[y][x] != CELL_DEAD) {
						sum += state[y][x] - CELL_MINALIVE;
						count++;
					}
				}
			}

			if (cellval != CELL_DEAD) {
				/*
				 * Survival of existing cell unless it has
				 * reached its maximum age.
				 * Note that count includes the cell itself.
				 */
				if ((count == 3 || count == 4) &&
				    (st->cellmaxage == 0 ||
				     ++cluster->cellage[cellY][cellX] < st->cellmaxage))
					continue;

				/* Otherwise, death. */
				cluster->cell[cellY][cellX] = CELL_DEAD;
				cluster->cellage[cellY][cellX] = 0;
				deaths++;

				changemapX |= 1 << cellX;
				changemapY |= 1 << cellY;

				continue;
			}

			if (count != 3)
				continue;

			/*
			 * Cell birth.
			 * Calculate color by averaging neighbors' plus some
			 * randomness.  The color index only increases until
			 * wrap-around.  Due to integer truncation, the odds
			 * of increasing the color are 1/4 (random % 8 must
			 * be either 6 or 7).
			 */
			color = ((sum << 1) + (random() % 0x07)) / 6;
			if (color >= st->colorwrap)
				color = 0;
			cluster->cell[cellY][cellX] = color + CELL_MINALIVE;
			births++;

			changemapX |= 1 << cellX;
			changemapY |= 1 << cellY;
		}
	}

	if (births == 0 && deaths == 0) {
		/* Dormant cluster. */
		if (cluster->numcells == 0)
			cluster->dormant++;
		return;
	}

	cluster->numcells += births - deaths;
	st->numcells += births - deaths;
#if 0
	fprintf(stderr, "[%p] births = %d, deaths = %d, numcells = %d\n",
			cluster, births, deaths, cluster->numcells);
#endif

	assert(cluster->numcells >= 0);
	cluster->dormant = 0;

	/*
	 * Finally, if there were any changes along the edges, wake the
	 * adjacent neighbor clusters because it will affect them too next
	 * iteration.  This isn't just an optimization: since
	 * life_state_update() only scans non-dormant clusters, if we didn't
	 * wake them then we would never detect spill-over at all.
	 */
	if (changemapY & (1 << 0)) {
		if (cluster->cell[0][0] != CELL_DEAD)
			life_cluster_wakeneighbor(st, cluster, -1, -1);	/* NW */
		life_cluster_wakeneighbor(st, cluster, 0, -1);		/* N */
		if (cluster->cell[0][CLUSTERSIZE-1] != CELL_DEAD)
			life_cluster_wakeneighbor(st, cluster, 1, -1);	/* NE */
	}
	if (changemapX & (1 << 0))
		life_cluster_wakeneighbor(st, cluster, -1 ,0);		/* W */
	if (changemapX & (1 << (CLUSTERSIZE-1))) 
		life_cluster_wakeneighbor(st, cluster, 1, 0);		/* E */
	if (changemapY & (1 << (CLUSTERSIZE-1))) {
		if (cluster->cell[0][CLUSTERSIZE-1] != CELL_DEAD)
			life_cluster_wakeneighbor(st, cluster, -1, 1);	/* SW */
		life_cluster_wakeneighbor(st, cluster, 0, 1);		/* S */
		if (cluster->cell[0][CLUSTERSIZE-1] != CELL_DEAD)
			life_cluster_wakeneighbor(st, cluster, 1, 1);	/* SE */
	}
}


struct cell_cluster *
life_cluster_new(struct state *st, int clusterX, int clusterY)
{
	struct cell_cluster **clustertable = st->clustertable;
	struct cell_cluster *cluster;
	struct cell_cluster *neighbor;
	int clusteridx;
	int neighboridx;
	int neighborX, neighborY;
	int x, y;

	int cluster_numX = st->cluster_numX;
	int cluster_numY = st->cluster_numY;

	if (clusterX == -1)
		clusterX += cluster_numX;
	if (clusterX == cluster_numX)
		clusterX = 0;

	if (clusterY == -1)
		clusterY += cluster_numY;
	if (clusterY == cluster_numY)
		clusterY = 0;

	assert(clusterX >= 0 && clusterX < cluster_numX);
	assert(clusterY >= 0 && clusterY < cluster_numY);

	clusteridx = (clusterY * cluster_numX) + clusterX;
	assert(clusteridx >= 0 && clusteridx < st->maxclusters);
	if ((cluster = clustertable[clusteridx]) != NULL) {
		/* Matches existing cluster; wake it if it is dormant. */
		cluster->dormant = 0;
		return (cluster);
	}

	cluster = calloc(1, sizeof(*cluster));
	if (cluster == NULL)
		exit (1);

	clustertable[clusteridx] = cluster;
	st->numclusters++;
	cluster->clusterX = clusterX;
	cluster->clusterY = clusterY;

	/*
	 * Cache pointers to neighboring clusters.  All universe-wrapping logic
	 * is done here so we don't have to do it in the speed-critical
	 * simulation loop.
	 */
	neighboridx = 0;
	for (y = clusterY - 1; y <= clusterY + 1; y++) {

		if (y < 0)
			neighborY = cluster_numY - 1;
		else if (y >= cluster_numY)
			neighborY = 0;
		else
			neighborY = y;

		for (x = clusterX - 1; x <= clusterX + 1; x++, neighboridx++) {
			if (x == clusterX && y == clusterY) {
				neighboridx--;
				continue;
			}

			if (x < 0)
				neighborX = cluster_numX - 1;
			else if (x >= cluster_numX)
				neighborX = 0;
			else
				neighborX = x;

			neighbor = clustertable[(neighborY * cluster_numX) +
						neighborX];
			cluster->neighbor[neighboridx] = neighbor;

			if (neighbor == NULL)
				continue;

			/*
			 * Update our neighbor with a link back to this new
			 * cluster.  This relies on direction (X) and direction
			 * (NUMDIRECTIONS - X - 1) being opposites.
			 * Note that with large cell sizes, clusters may be
			 * their own neighbor.
			 */
			assert(neighbor->neighbor[NUMDIRECTIONS - 1 - neighboridx] == NULL ||
			       neighbor->neighbor[NUMDIRECTIONS - 1 - neighboridx] == cluster);
			neighbor->neighbor[NUMDIRECTIONS - 1 - neighboridx] = cluster;
		}
	}

	return (cluster);
}


void
life_cluster_delete(struct state *st, struct cell_cluster *cluster)
{
	struct cell_cluster *neighbor;
	int clusteridx;
	int neighboridx;

	assert(cluster->numcells == 0);
	assert(st->numclusters > 0);

	for (neighboridx = 0; neighboridx < NUMDIRECTIONS; neighboridx++) {
		neighbor = cluster->neighbor[neighboridx];
		if (neighbor == NULL)
			continue;

		/*
		 * Clear our neighbors' link to this cluster.  This relies on
		 * direction (X) and direction (NUMDIRECTIONS - X - 1) being
		 * opposites.
		 */
		assert(neighbor->neighbor[NUMDIRECTIONS - 1 - neighboridx] != NULL);
		neighbor->neighbor[NUMDIRECTIONS - 1 - neighboridx] = NULL;
	}

	clusteridx = (cluster->clusterY * st->cluster_numX) +
		     cluster->clusterX;
	st->clustertable[clusteridx] = NULL;
	st->numclusters--;
	free(cluster);
}


static
void
life_cluster_draw(const struct state * const st, Display *dpy, Window window,
		  const struct cell_cluster * const cluster,
		  int xstart, int ystart)
{
	const XColor *trailcolors = st->trailcolors;
	const XColor *colors = st->colors;
	int xoffset, yoffset;
	int cellX, cellY;
	int cellidx;

	cellidx = 0;
	for (cellY = 0, yoffset = ystart;
	     cellY < CLUSTERSIZE;
	     cellY++, yoffset += st->cellsize) {

		for (cellX = 0, xoffset = xstart;
		     cellX < CLUSTERSIZE;
		     cellX++, xoffset += st->cellsize) {

			GC context = st->gc_draw;
			cell c = cluster->cell[cellY][cellX];

			if (c == CELL_DEAD) {
				c = cluster->oldcell[cellY][cellX];
				if (c == CELL_DEAD)
					continue;
				if (trailcolors != NULL) {
					XSetForeground(dpy, st->gc_draw,
						       trailcolors[c].pixel);
				} else {
					context = st->gc_erase;
				}
			} else {
				/* Live cell. */
				XSetForeground(dpy, st->gc_draw,
					       colors[c].pixel);
			}

			XFillRectangle(dpy, st->buf, context, xoffset, yoffset,
				       st->celldrawsize, st->celldrawsize);
		}
	}
}


/*
 * life_cell_set() - Sets a cell to alive.
 *
 *	This interface, while general, is horribly inefficient.  It is only
 *	used to draw patterns which are specified by coordinates.  Anything
 *	in the fast-path should avoid this routine like the plague.
 *
 *	In this case, color should be a number between 0 and numcolors.  We'll
 *	take care of adjusting it to avoid the CELL_DEAD "color".
 */
void
life_cell_set(struct state *st, int x, int y, int color)
{
	struct cell_cluster *cluster;
	int clusterX, clusterY;

	/*
	 * First, handle wrapping of the X and Y coordinates.
	 */
	while (x < 0)
		x += st->cell_numX;
	while (x >= st->cell_numX)
		x -= st->cell_numX;
	while (y < 0)
		y += st->cell_numY;
	while (y >= st->cell_numY)
		y -= st->cell_numY;

	/*
	 * Handle wrapping of the color coordinate also.
	 * Note that this routine cannot be called to kill cells.
	 */
	while (color <= CELL_MINALIVE)
		color += st->numcolors;
	while (color >= st->colorwrap)
		color -= st->colorwrap;

	/* Now convert into <cluster, cell> coordinates. */
	clusterX = x / CLUSTERSIZE;
	clusterY = y / CLUSTERSIZE;
	x = x % CLUSTERSIZE;
	y = y % CLUSTERSIZE;

	cluster = life_cluster_new(st, clusterX, clusterY);

	if (cluster->cell[y][x] != CELL_DEAD) {
		/* Already a cell there.  Let's merge them. */
		cluster->cell[y][x] = CELL_MINALIVE +
			((cluster->cell[y][x] - CELL_MINALIVE + color) / 2);
		return;
	}

	cluster->cell[y][x] = color + CELL_MINALIVE;
	cluster->numcells++;
	st->numcells++;

	cluster->dormant = 0;
	if (y == 0) {
		if (x == 0)
			life_cluster_wakeneighbor(st, cluster, -1, -1);	/* NW */
		life_cluster_wakeneighbor(st, cluster, 0, -1);		/* N */
		if (x == CLUSTERSIZE - 1)
			life_cluster_wakeneighbor(st, cluster, 1, -1);	/* NE */
	}
	if (x == 0)
		life_cluster_wakeneighbor(st, cluster, -1 ,0);		/* W */
	if (x == CLUSTERSIZE - 1) 
		life_cluster_wakeneighbor(st, cluster, 1, 0);		/* E */
	if (y == CLUSTERSIZE - 1) {
		if (x == 0)
			life_cluster_wakeneighbor(st, cluster, -1, 1);	/* SW */
		life_cluster_wakeneighbor(st, cluster, 0, 1);		/* S */
		if (x == CLUSTERSIZE - 1)
			life_cluster_wakeneighbor(st, cluster, 1, 1);	/* SE */
	}
}


void
life_pattern_init(struct state *st, Display *dpy)
{
	char *patternfiles[NUMPATTERNS];
	char *pattern_path;
	char *pattern_dir;
	int count;
	int pos;
	size_t len;

	count = 0;
	memset(patternfiles, 0, sizeof(patternfiles));

	/*
	 * First, build a list of NUMPATTERNS files to read from.  It is
	 * possible that not all of the files contain usable patterns; we have
	 * some built-in patterns, though.
	 */
	pattern_path = get_string_resource(dpy, "patternPath", "String");
	while ((pattern_dir = strsep(&pattern_path, ":")) != NULL) {
		struct dirent *entry;
		DIR *dir;

		/* Trim trailing slashes off of the directory name. */
		len = strlen(pattern_dir);
		while (len > 0 && pattern_dir[len - 1] == '/') {
			pattern_dir[len - 1] = '\0';
			len--;
		}

		dir = opendir(pattern_dir);
		if (dir == NULL)
			continue;

		while ((entry = readdir(dir)) != NULL) {
			if (entry->d_type != DT_REG)
				continue;

			pos = random() % (count + 2);	/* XXX Magic Hack */
			if (pos >= NUMPATTERNS)
				continue;

			if (patternfiles[pos] != NULL)
				free(patternfiles[pos]);

			len = strlen(pattern_dir) + 1 + entry->d_namlen + 1;
			patternfiles[pos] = malloc(len);
			if (patternfiles[pos] == NULL)
				continue;

			snprintf(patternfiles[pos], len, "%s/%s", pattern_dir,
				 entry->d_name);
			count++;
		}

		closedir(dir);
	}

	/*
	 * Initialize pattern list with built-in patterns.
	 */
	memcpy(st->patterns, builtin_patterns, sizeof(builtin_patterns));

	/*
	 * Now, try to load a pattern from each of the selected files until we
	 * have NUMPATTERNS (including builtins).
	 */
	count = NUMPATTERNSBUILTIN;
	pos = 0;
	while (count < NUMPATTERNS && pos < NUMPATTERNS) {
		if (life_pattern_read(st, patternfiles[pos],
				      &st->patterns[count])) {
			count++;
#ifdef LIFE_PRINTPATTERNS
			fprintf(stderr, "Loaded pattern %s\n",
				patternfiles[pos]);
#endif
		}
		pos++;
	}

	/* Pad out empty entries in the pattern array. */
	for (pos = 0; count < NUMPATTERNS; pos++, count++)
		st->patterns[count] = st->patterns[pos];

	/* Free memory allocated to filenames. */
	for (pos = 0; pos < NUMPATTERNS; pos++) {
		if (patternfiles[pos] != NULL)
			free(patternfiles[pos]);
	}
}


void
life_pattern_free(struct state *st)
{
	struct coords *coords0;
	int i;

	/*
	 * The pattern array is always padded out to NUMPATTERNS entries
	 * by duplicating patterns as necessary.  In addition, the first
	 * NUMPATTERNSBUILTIN patterns are static and do not need freeing.
	 * As such, we free the patterns starting at index NUMPATTERNSBUILTIN
	 * and continueing through the array until we see pattern #0
	 * duplicated or we free NUMPATTERNS, whichever comes first.
	 */
	coords0 = st->patterns[0].coords;

	for (i = NUMPATTERNSBUILTIN; i < NUMPATTERNS; i++) {
		if (st->patterns[i].coords == coords0)
			break;
		free(st->patterns[i].coords);
	}
}


static __inline
int
randbit(void)
{
	static unsigned int randbits;
	static int randcount = 0;
	int rv;

	if (randcount == 0) {
		randbits = random();
		randcount = 32;		/* Good for 32 bits. */
	}

	rv = randbits & 0x01;
	randbits >>= 1;
	return (rv);
}


void
life_pattern_draw(struct state *st)
{
	struct cell_cluster *cluster;
	struct pattern *pattern;
	const struct coords *coord, *endcoord;
	int cellX, cellY;
	int color;
	int clusterX, clusterY;
	int clusteridx;
	int lastneeded;
	int tries;

	/*
	 * First, find an empty cluster.
	 * We don't strictly need an empty cluster, but finding one is a good
	 * sign of a fairly sparsly populated region of the screen.
	 */
	for (tries = 5; tries > 0; tries--) {
		clusteridx = random() % st->maxclusters;
		cluster = st->clustertable[clusteridx];
		if (cluster == NULL)
			break;
	}

	/* Didn't find an empty cluster.  Hope for better luck next time... */
	if (tries == 0)
		return;

	clusterY = clusteridx / st->cluster_numX;
	clusterX = clusteridx % st->cluster_numX;
	cellY = (clusterY * CLUSTERSIZE) + (random() % CLUSTERSIZE);
	cellX = (clusterX * CLUSTERSIZE) + (random() % CLUSTERSIZE);

	/*
	 * Pick a random pattern.
	 * Tries up to 5 times to find a pattern that fits in the amount of
	 * empty space available.  Since we don't yet know how the pattern
	 * will be rotated, check both dimensions for the maximum required
	 * space.
	 */
	lastneeded = INT_MAX;
	for (tries = 5; tries > 0; tries--) {
		int needX, needY, needed;
		int scanX, scanY;

		pattern = &st->patterns[random() % NUMPATTERNS];

		needX = ((cellX % CLUSTERSIZE) + pattern->width) / CLUSTERSIZE;
		needY = ((cellY % CLUSTERSIZE) + pattern->height) / CLUSTERSIZE;
		needed = needX > needY ? needX : needY;

		if (needed == 1)	/* Fits in initial cluster. */
			break;

		/*
		 * If this pattern is the same size or larger than the previous
		 * then we don't have a chance of succeeding.
		 */
		if (needed >= lastneeded)
			goto noFit;

		for (needY = 1; needY < needed; needY++) {
			scanY = clusterY + needY;
			if (scanY >= st->cluster_numY)
				scanY -= st->cluster_numY;

			for (needX = 1; needX < needed; needX++) {
				scanX = clusterX + needX;
				if (scanX >= st->cluster_numX)
					scanX -= st->cluster_numX;

				clusteridx = (scanY * st->cluster_numX) +
					     scanX;
			        assert(clusteridx >= 0 &&
				       clusteridx < st->maxclusters);
				cluster = st->clustertable[clusteridx];

				if (cluster != NULL && cluster->numcells > 0)
					goto noFit;
			}
		}

		/* We found a pattern which fits in the empty region. */
		break;

noFit:
		/*
		 * This pattern won't fit, try another pattern in the list.
		 */
		lastneeded = needed;

	}
	coord = pattern->coords;
	endcoord = pattern->coords + pattern->numcoords;

	/*
	 * Write pattern.
	 */

	color = random() % st->numcolors;

	switch (random() % 4) {
	case 0:
		for (; coord < endcoord; coord++) {
			life_cell_set(st, cellX + coord->x,
					  cellY + coord->y, color);
			color += randbit();
		}
		break;

	case 1:
		/* Rotate 90 degrees. */
		for (; coord < endcoord; coord++) {
			life_cell_set(st, cellX + coord->y,
				          cellY + coord->x, color);
			color += randbit();
		}
		break;

	case 2:
		/* Flip vertically. */
		for (; coord < endcoord; coord++) {
			life_cell_set(st, cellX + coord->x,
					  cellY + pattern->width - coord->y,
					  color);
			color += randbit();
		}
		break;

	case 3:
		/* Rotate -90 degrees. */
		for (; coord < endcoord; coord++) {
			life_cell_set(st, cellX + pattern->width - coord->y,
					  cellY + coord->x, color);
			color += randbit();
		}
		break;

	default:
		assert(0);
		/* NOTREACHED */
	}
}


/*
 * life_pattern_read() - Parse a pattern stored in a Life 1.05 format file.
 *
 *	If the file is an unknown format or the pattern is too large, then
 *	returns boolean false.  Otherwise, returns boolean true and populates
 *	the given pattern structure with data from the file.
 *	A good collection of Life 1.05 format patterns can be found at
 *		http://www.ibiblio.org/lifepatterns/#patterns
 */
int
life_pattern_read(const struct state * const st, const char *filename,
		  struct pattern *pattern)
{
	struct coords coordbuf[PATTERN_MAXCOORDS];
	struct coords linecoord, mincoord, maxcoord;
	struct coords *coord;

	char line[128];
	char *pos, *endptr;
	FILE *f;
	size_t len;

	maxcoord.x = maxcoord.y = INT_MIN;
	mincoord.x = mincoord.y = INT_MAX;

	f = fopen(filename, "r");
	if (f == NULL)
		return (False);

	pattern->numcoords = 0;
	coord = coordbuf;

	while (fgets(line, sizeof(line), f) != NULL) {
		if (line[0] == '#') {
			/* We only (barely) understand #P lines. */
			if (line[1] != 'P')
				continue;

			linecoord.x = strtol(line + 2, &endptr, 10);
			linecoord.y = strtol(endptr, &endptr, 10);
			coord->y = linecoord.y;

			if (linecoord.x < mincoord.x)
				mincoord.x = linecoord.x;
			if (linecoord.y < mincoord.y)
				mincoord.y = linecoord.y;
			continue;
		}

		coord->x = linecoord.x;
		for (pos = line; *pos != '\0'; pos++) {
			if (*pos == '*') {
				if (coord->x > maxcoord.x)
					maxcoord.x = coord->x;
				if (coord->y > maxcoord.y)
					maxcoord.y = coord->y;

				coord++;
				pattern->numcoords++;
				if (pattern->numcoords == PATTERN_MAXCOORDS) {
					/* Too many coordinates in pattern. */
					fclose(f);
					return (False);
				}

				/* Initialize the next coordinate. */
				*coord = *(coord - 1);
			}
			coord->x++;
		}
		coord->y++;
	}

	fclose(f);

	/* Ignore empty patterns. */
	if (pattern->numcoords == 0)
		return (False);

	/*
	 * Calculate the width of the pattern.
	 * This is actually 1 less than the width, but every it is used had to
	 * subtract 1 to get a useable value, so we just subtract the one here
	 * and be done with it.
	 */
	pattern->width = maxcoord.x - mincoord.x;
	pattern->height = maxcoord.y - mincoord.y;

	/*
	 * Don't bother with patterns which are too big to be displayed in
	 * any meaningful manner.
	 */
	if (pattern->width > st->cell_numX / 2 ||
	    pattern->height > st->cell_numY / 2)
		return (False);

	/*
	 * Normalize the pattern's cell coordinates such that the minimum X
	 * and Y values are both 0.
	 */
	for (; coord > coordbuf; coord--) {
		coord->x -= mincoord.x;
		coord->y -= mincoord.y;
	}

	/* Finally, copy the coordinate data into the pattern record. */
	len = sizeof(struct coords) * pattern->numcoords;
	pattern->coords = malloc(len);
	if (pattern->coords == NULL)
		return (False);
	memcpy(pattern->coords, coordbuf, len);
	return (True);
}


void
life_display_init(struct state *st, Display *dpy, Window window)
{
	XGCValues gcv;
	int leavetrails;
	int i;

	st->delay = get_integer_resource(dpy, "delay", "Integer");
	if (st->delay < 0)
		st->delay = 0;

	st->numcolors = get_integer_resource(dpy, "ncolors", "Integer");
	if (st->numcolors < 2)
		st->numcolors = 2;

	leavetrails = get_boolean_resource(dpy, "trails", "Boolean");
	if (st->numcolors == 2)
		leavetrails = False;

	st->double_buffer = get_boolean_resource(dpy, "doubleBuffer", "Boolean");
	st->DBEclear = get_boolean_resource(dpy, "useDBEClear", "Boolean");

	XGetWindowAttributes(dpy, window, &st->xgwa);


	/*
	 * Allocate colors for live cells and, optionally, cell trails.
	 * Note that in both maps we leave index 0 as NULL.  We use index 0 as
	 * a dead-cell identifier so a) we want to catch references to it as a
	 * color and b) otherwise we would waste an allocated color.
	 */
	if (leavetrails) {
		/*
		 * Allocate half of ncolors to the color trails and the other
		 * half to the live cells.
		 */
		st->numcolors /= 2;
	}

	if (st->numcolors > CELL_MAXCOLORS)
		st->numcolors = CELL_MAXCOLORS;

	/*
	 * Allocate 3 times as many color pointers as we allocate colors.
	 * When a new cell is born, we average the color of the 3 neighbors
	 * (plus some randomness).  However, when the neighbors' colors
	 * approach numcolors, the newborn's color may wrap back to 0.  The
	 * next newborn will average the 0 with the other surviving cells and
	 * get a value of (numcolors * 2/3).  Even with a closed color map,
	 * this causes a visual discontinuity that is really obvious with
	 * gliders.  To work around the problem, we make the color map repeat
	 * 3 times so that, despite the numeric jump, there is no visual
	 * incoherence.
	 */
	st->colors = calloc(sizeof(XColor),
			    (st->numcolors + CELL_MINALIVE) * 3);
	if (leavetrails) {
		st->trailcolors = calloc(sizeof(XColor),
					 (st->numcolors + CELL_MINALIVE) * 3);
	} else
		st->trailcolors = NULL;

alloccolors:
	/* Main color gradient: used for drawing live cells. */
	make_color_ramp(dpy, st->xgwa.colormap,
			0, 1, 1,
			360, 1, 1,
			&st->colors[CELL_MINALIVE], &st->numcolors,
			False	/* closed */,
			True	/* allocate */,
			False	/* writable */);

	/* Make dimmer variants for trails. */
	if (st->trailcolors != NULL) {
		double saturation, value;
		int hue;

		for (i = 1; i <= st->numcolors; i++) {
			rgb_to_hsv(st->colors[i].red,
				   st->colors[i].green,
				   st->colors[i].blue,
				   &hue, &saturation, &value);
			hsv_to_rgb(hue, saturation, value * 0.40,
				   &st->trailcolors[i].red,
				   &st->trailcolors[i].green,
				   &st->trailcolors[i].blue);

			if (XAllocColor(dpy, st->xgwa.colormap,
					&st->trailcolors[i]))
				continue;
			/*
			 * Error occurred allocating color.  Reduce the number
			 * we are trying to allocate and try again.
			 */
			free_colors(dpy, st->xgwa.colormap,
				    st->colors, st->numcolors);
			free_colors(dpy, st->xgwa.colormap,
				    st->trailcolors, i);

			st->numcolors--;
			if (st->numcolors <= 0)
				exit (1);
			goto alloccolors;	/* XXX Evil. */
		}
	}

	/*
	 * Duplicate color pointers so that all colors appear in the list(s)
	 * 3 times.
	 */
	st->colorwrap = st->numcolors * 3;
	for (i = 0; i < st->numcolors; i++) {
		st->colors[(st->numcolors * 2) + i + CELL_MINALIVE] =
		    st->colors[st->numcolors + i + CELL_MINALIVE] =
		    st->colors[i + CELL_MINALIVE];
	}
	if (st->trailcolors != NULL) {
		for (i = 0; i < st->numcolors; i++) {
			st->trailcolors[(st->numcolors * 2) + i + CELL_MINALIVE] =
			    st->trailcolors[st->numcolors + i + CELL_MINALIVE] =
			    st->trailcolors[i + CELL_MINALIVE];
		}
	}

	st->pixmap = None;
	st->buf = None;
	st->backbuf = None;

	if (st->double_buffer) {
#ifdef HAVE_DOUBLE_BUFFER_EXTENSION
		st->backbuf = xdbe_get_backbuffer(dpy, window,
				   st->DBEclear ? XdbeBackground
						: XdbeUndefined);
		st->buf = st->backbuf;
#endif /* HAVE_DOUBLE_BUFFER_EXTENSION */

		/*
		 * If we failed to get a back buffer from the double-buffer
		 * extension, or if the extension is unavailable, fall back
		 * to software double-buffering.
		 */
		if (st->buf == None) {
			st->buf = XCreatePixmap(dpy, window, st->xgwa.width,
							     st->xgwa.height,
							     st->xgwa.depth);
			st->pixmap = st->buf;
		}
	}
	if (st->buf == None) {
		/* If not double buffering, just draw directly to the window. */
		st->buf = window;
	}

	gcv.foreground = get_pixel_resource(dpy, st->xgwa.colormap,
					    "background", "Background");
	st->gc_erase = XCreateGC(dpy, st->buf, GCForeground, &gcv);

	gcv.background = gcv.foreground;
	gcv.foreground = get_pixel_resource(dpy, st->xgwa.colormap,
					    "foreground", "Foreground");
	st->gc_draw = XCreateGC(dpy, st->buf, GCForeground|GCBackground, &gcv);

	XFillRectangle(dpy, st->buf, st->gc_erase, 0, 0,
		       st->xgwa.width, st->xgwa.height);

	/*
	 * Precompute variables used in life_display_update() which do not
	 * change during execution.
	 */
#ifdef HAVE_DOUBLE_BUFFER_EXTENSION
	if (st->backbuf != None) {
		st->swapinfo.swap_window = window;
		st->swapinfo.swap_action = XdbeCopied;
	}
#endif
}


void
life_display_free(struct state *st, Display *dpy)
{

	free_colors(dpy, st->xgwa.colormap, st->colors, st->numcolors);
	free(st->colors);

	if (st->trailcolors != NULL) {
		free_colors(dpy, st->xgwa.colormap, st->trailcolors,
			    st->numcolors);
		free(st->trailcolors);
	}

	if (st->pixmap != None)
		XFreePixmap(dpy, st->pixmap);
}


void
life_display_update(struct state *st, Display *dpy, Window window)
{
	struct cell_cluster ** clustertable = st->clustertable;
	struct cell_cluster *cluster;
	int clusterX, clusterY;
	int clusteridx;
	int xoffset, yoffset;

	int clustersize = st->cellsize * CLUSTERSIZE;

	/*
	 * Draw cells.
	 */
	clusteridx = 0;
	for (clusterY = 0, yoffset = st->display_offsetY;
	     clusterY < st->cluster_numY;
	     clusterY++, yoffset += clustersize) {

		for (clusterX = 0, xoffset = st->display_offsetX;
		     clusterX < st->cluster_numX;
		     clusterX++, xoffset += clustersize,
				 clusteridx++) {

			cluster = clustertable[clusteridx];
			if (cluster == NULL || cluster->dormant > LIMIT_DRAW)
				continue;

			life_cluster_draw(st, dpy, window, cluster,
					  xoffset, yoffset);

			/*
			 * Now that we've drawn the state; record it as the old
			 * state so we can calculate the next iteration.
			 */
			memcpy(cluster->oldcell, cluster->cell,
			       sizeof(cluster->cell));
		}
	}

	/*
	 * Switch draw buffer to display buffer (if double-buffering).
	 */
#ifdef HAVE_DOUBLE_BUFFER_EXTENSION
	if (st->backbuf != None) {
		XdbeSwapBuffers(dpy, &st->swapinfo, 1);
		return;
	}
#endif
	if (st->double_buffer) {
		/* We are doing software double-buffering. */
		XCopyArea(dpy, st->buf, window, st->gc_draw, 0, 0,
			  st->xgwa.width, st->xgwa.height, 0, 0);
	}
}


#ifdef LIFE_SHOWGRID
static
void
life_display_grid(const struct state * const st, Display *dpy)
{
	int i;
	int s = CLUSTERSIZE * st->cellsize;
	for (i = 1; i < st->cluster_numY; i++) {
		int pos = (i * s) + st->display_offsetY - 1;
		XDrawLine(dpy, st->buf, st->gc_draw,
			  st->display_offsetX, pos,
			  st->xgwa.width - st->display_offsetX, pos);
	}
	for (i = 1; i < st->cluster_numX; i++) {
		int pos = (i * s) + st->display_offsetY - 1;
		XDrawLine(dpy, st->buf, st->gc_draw,
			  pos, st->display_offsetY,
			  pos, st->xgwa.height - st->display_offsetY);
	}
}
#endif


const char *progclass = "CLife";

const char *life_hack_defaults [] = {
	".background:		black",
	".foreground:		white",
	"*delay:		25000",
	"*ncolors:		100",
	"*maxAge:		255",
	"*cellSize:		5",
	"*cellBorder:		True",
	"*trails:		True",
	"*doubleBuffer:		True",
#ifdef HAVE_DOUBLE_BUFFER_EXTENSION
	"*useDBE:		True",
	"*useDBEClear:		True",
#endif /* HAVE_DOUBLE_BUFFER_EXTENSION */
	NULL
};

const XrmOptionDescRec life_hack_options [] = {
	{ "-delay",		".delay",	XrmoptionSepArg, NULL },
	{ "-ncolors",		".ncolors",	XrmoptionSepArg, NULL },
	{ "-maxage",		".maxage",	XrmoptionSepArg, NULL },
	{ "-cellsize",		".cellSize",	XrmoptionSepArg, NULL },
	{ "-cellborder",	".cellBorder",	XrmoptionNoArg, "True" },
	{ "-no-cellborder",	".cellBorder",	XrmoptionNoArg, "False" },
	{ "-trails",		".trails",	XrmoptionNoArg, "True" },
	{ "-no-trails",		".trails",	XrmoptionNoArg, "False" },
	{ "-db",		".doubleBuffer", XrmoptionNoArg, "True" },
	{ "-no-db",		".doubleBuffer", XrmoptionNoArg, "False" },
	{ "-patterns",		".patternPath", XrmoptionSepArg, NULL },
	{ 0, 0, 0, 0 }
};

static
void *
life_hack_init(Display *dpy, Window window)
{
	struct state *st = (struct state *)calloc(1, sizeof(*st));

	life_display_init(st, dpy, window);
	life_state_init(st, dpy);
	life_pattern_init(st, dpy);

#ifdef LIFE_SHOWGRID
	life_display_grid(st, dpy);
#endif
	return st;
}


static
unsigned long
life_hack_draw(Display *dpy, Window window, void *closure)
{
	struct state *st = (struct state *)closure;

	life_display_update(st, dpy, window);
	life_state_update(st);
	return st->delay;
}


static
void
life_hack_reshape(Display *dpy, Window window, void *closure,
		  unsigned int w, unsigned int h)
{
	struct state *st = (struct state *)closure;

	XGetWindowAttributes(dpy, window, &st->xgwa);
	/* XXX Need to recalculate clusters/cells. */
}


static
Bool
life_hack_event(Display *dpy, Window window, void *closure, XEvent *event)
{
	return False;
}


static
void
life_hack_free(Display *dpy, Window window, void *closure)
{
	struct state *st = (struct state *)closure;

	life_pattern_free(st);
	life_state_free(st);
	life_display_free(st, dpy);
	free(st);
}


XSCREENSAVER_MODULE ("CLife", life_hack)
