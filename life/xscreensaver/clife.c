/*
 * Copyright (c) 2003 Kelly Yancey (kbyanc@posi.net)
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation.  No representations are made about the suitability of this
 * software for any purpose.  It is provided "as is" without express or 
 * implied warranty.
 *
 * $kbyanc: life/xscreensaver/clife.c,v 1.1 2003/08/13 00:27:45 kbyanc Exp $
 */

/* Undefine the following before testing any code changes! */
#define NDEBUG

#include <assert.h>
#include "screenhack.h"

#ifdef HAVE_DOUBLE_BUFFER_EXTENSION
# include "xdbe.h"
#endif /* HAVE_DOUBLE_BUFFER_EXTENSION */

/*
 * Display parameters.
 */
static int	 delay;
static int	 numcolors;
static int	 leavetrails;
static int	 double_buffer;
static int	 DBEclear;	/* Use DBE to clear buffer. */
static int	 Xclear;	/* Use XFillRectangle to clear buffer. */

/* Internal display variables. */
static XWindowAttributes xgwa;
static XColor	 *colors;
static XColor	 *trailcolors;
static GC	  gc_erase;
static GC	  gc_draw;
static Pixmap	  bufpage[2];	/* For software double-buffering. */
static Pixmap	  buf = NULL;	/* Current work buffer. */
static int	  display_offsetX, display_offsetY;

#ifdef HAVE_DOUBLE_BUFFER_EXTENSION
static XdbeBackBuffer backbuf;
static XdbeSwapInfo swapinfo;
#endif


/*
 * Simulation parameters.
 */
static int	 cellsize;

/* Internal simulation variables. */
#define	CELLPAD		1	/* Number of pixels between displayed cells. */
#define	CLUSTERSIZE	8	/* Number of cells per cluster; power-of-2. */
#define	MAXDORMANTAGE	10	/* How long a cluster can be static before we
				 * consider it dormant.
				 */

typedef	unsigned char cell;
#define	CELL_DEAD	0
#define	CELL_MINALIVE	1
#define	CELL_MAXALIVE	255

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
struct cell_cluster {
	short			 numcells;
	unsigned char		 dormant;	/* Iterations unchanged. */
	unsigned char		 reserved;
	int			 clusterX, clusterY;
	struct cell_cluster	*neighbor[NUMDIRECTIONS];
	cell			 oldcell[CLUSTERSIZE][CLUSTERSIZE];
	cell			 cell[CLUSTERSIZE][CLUSTERSIZE];
};

static struct cell_cluster **clustertable;

static int	 numclusters;
static int	 numcells;
static int	 maxclusters;
static int	 maxcells;
static int	 cluster_numX, cluster_numY;
static int	 cell_numX, cell_numY;


static struct cell_cluster *life_cluster_new(int clusterX, int clusterY);
static void	 life_cluster_delete(struct cell_cluster *cluster);
static void	 life_cluster_update(struct cell_cluster *cluster);
static void	 life_cluster_draw(Display *dpy, Window window,
				   struct cell_cluster *cluster,
				   int xoffset, int yoffset);
static void	 life_cluster_wakeneighbors(struct cell_cluster *cluster);
static void	 life_cell_set(int x, int y, int color);

static void	 life_random_pattern(void);

static void	 life_state_init(void);
static void	 life_state_update(void);

static void	 life_display_init(Display *dpy, Window window);
static void	 life_display_update(Display *dpy, Window window);


static struct cell_cluster *life_cluster_new(int clusterX, int clusterY);


void
life_state_init(void)
{
	int displaysize;

	cellsize = get_integer_resource("cellsize", "Integer");
	if (cellsize < 1)
		cellsize = 1;

	for (;;) {
		displaysize = cellsize + CELLPAD;
		cell_numX = xgwa.width / displaysize;
		cell_numY = xgwa.height / displaysize;

		cluster_numX = cell_numX / CLUSTERSIZE;
		cluster_numY = cell_numY / CLUSTERSIZE;

		if (cluster_numX >= 1 && cluster_numY >= 1)
			break;

		/*
		 * The cells are too big to fit at least 1 cluster on the
		 * display.  Reduce the cell size until a cluster fits.
		 */
		cellsize--;
		if (cellsize < 0)
			exit(1);
	}

	/*
	 * Recompute the number of cells as a multiple of the number of
	 * clusters.
	 */
	cell_numX = cluster_numX * CLUSTERSIZE;
	cell_numY = cluster_numY * CLUSTERSIZE;

	/* Center the cell display. */
	display_offsetX = (xgwa.width - (cell_numX * displaysize)) / 2;
	display_offsetY = (xgwa.height - (cell_numY * displaysize)) / 2;

	/*
	 * Allocate the cluster lookup table.  Initialize all pointers to
	 * NULL to indicate an empty universe.
	 */
	maxcells = cell_numX * cell_numY;
	maxclusters = cluster_numX * cluster_numY;
	clustertable = calloc(maxclusters, sizeof(*clustertable));
	if (clustertable == NULL)
		exit(1);
	numcells = 0;
	numclusters = 0;
}


void
life_state_update(void)
{
	struct cell_cluster *cluster;
	int clusteridx;
	int numactive;

	numactive = 0;
	for (clusteridx = 0; clusteridx < maxclusters; clusteridx++) {
		cluster = clustertable[clusteridx];
		if (cluster == NULL)
			continue;
		if (cluster->dormant < MAXDORMANTAGE) {
			life_cluster_update(cluster);
			numactive++;
			continue;
		}
		if (cluster->numcells == 0) {
			life_cluster_delete(cluster);
			continue;
		}
	}

	/* Try to keep the display at least 6.25% full. */
	if (numclusters * 16 < maxclusters || random() % 128 == 0)
		life_random_pattern();

#if 0
	fprintf(stderr, "%03d/%03d clusters (%03d active); %05d/%05d cells\n",
			numclusters, maxclusters, numactive, numcells, maxcells);
#endif
}


void
life_cluster_update(struct cell_cluster *cluster)
{
	static cell state[CLUSTERSIZE + 2][CLUSTERSIZE + 2];
	struct cell_cluster *neighbor;
	int cellX, cellY;
	int x, y, count;
	int occupied;
	int deaths, births;
	int edgechange;
	int sum;
	int idx;

	deaths = births = edgechange = 0;
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
	for (cellY = 1; cellY < CLUSTERSIZE + 1; cellY++) {
		for (cellX = 1; cellX < CLUSTERSIZE + 1; cellX++) {

			occupied = state[cellY][cellX] != CELL_DEAD;
			count = occupied ? -1 : 0;
			sum = 0;

			for (y = cellY - 1; y <= cellY + 1; y++) {
				for (x = cellX - 1; x <= cellX + 1; x++) {
					if (state[y][x] != CELL_DEAD) {
						sum += state[y][x] - CELL_MINALIVE;
						count++;
					}
				}
			}

			if (count == 2)
				continue;	/* No change. */

			if (count != 3 && occupied) {
				/* Cell death if occupied. */
				cluster->cell[cellY - 1][cellX - 1] = CELL_DEAD;
				deaths++;

				if (cellX == 1 || cellY == 1 ||
				    cellX == CLUSTERSIZE ||
				    cellY == CLUSTERSIZE)
					edgechange++;

				continue;
			}
			if (count != 3)
				continue;

			if (count == 3 && !occupied) {
				/*
				 * Cell birth unless occupied.
				 * Calculate color by averaging neighbors' plus
				 * some randomness.  The color index only
				 * increases until wrap-around.
				 */
				int color = (sum + random() % 4) / 3;
				if (color >= numcolors)
					color -= numcolors;
				cluster->cell[cellY - 1][cellX - 1] = color + CELL_MINALIVE;
				births++;

				if (cellX == 1 || cellY == 1 ||
				    cellX == CLUSTERSIZE ||
				    cellY == CLUSTERSIZE)
					edgechange++;
			}
		}
	}

	if (births == 0 && deaths == 0) {
		/* Dormant cluster. */
		cluster->dormant++;
		return;
	}

	cluster->numcells += births - deaths;
	numcells += births - deaths;
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
	if (edgechange != 0)
		life_cluster_wakeneighbors(cluster);
}


void
life_cluster_wakeneighbors(struct cell_cluster *cluster)
{
	struct cell_cluster *neighbor;
	int cellX, cellY;

	if (cluster->cell[0][0] != CELL_DEAD) {
		neighbor = life_cluster_new(cluster->clusterX - 1,
					    cluster->clusterY - 1);
		neighbor->dormant = 0;
	}

	if (cluster->cell[0][CLUSTERSIZE-1] != CELL_DEAD) {
		neighbor = life_cluster_new(cluster->clusterX + 1,
					     cluster->clusterY - 1);
		neighbor->dormant = 0;
	}

	if (cluster->cell[CLUSTERSIZE-1][0] != CELL_DEAD) {
		neighbor = life_cluster_new(cluster->clusterX - 1,
					    cluster->clusterY + 1);
		neighbor->dormant = 0;
	}

	if (cluster->cell[CLUSTERSIZE-1][CLUSTERSIZE-1] != CELL_DEAD) {
		neighbor = life_cluster_new(cluster->clusterX + 1,
					    cluster->clusterY + 1);
		neighbor->dormant = 0;
	}

	for (cellX = 0; cellX < CLUSTERSIZE; cellX++) {
		if (cluster->cell[0][cellX] == CELL_DEAD)
			continue;
		neighbor = life_cluster_new(cluster->clusterX,
					    cluster->clusterY - 1);
		neighbor->dormant = 0;
		break;
	}

	for (cellX = 0; cellX < CLUSTERSIZE; cellX++) {
		if (cluster->cell[CLUSTERSIZE-1][cellX] == CELL_DEAD)
			continue;
		neighbor = life_cluster_new(cluster->clusterX,
					    cluster->clusterY + 1);
		neighbor->dormant = 0;
		break;
	}

	for (cellY = 0; cellY < CLUSTERSIZE; cellY++) {
		if (cluster->cell[cellY][0] == CELL_DEAD)
			continue;
		neighbor = life_cluster_new(cluster->clusterX - 1,
					    cluster->clusterY);
		neighbor->dormant = 0;
		break;
	}

	for (cellY = 0; cellY < CLUSTERSIZE; cellY++) {
		if (cluster->cell[cellY][CLUSTERSIZE-1] == CELL_DEAD)
			continue;
		neighbor = life_cluster_new(cluster->clusterX + 1,
					    cluster->clusterY);
		neighbor->dormant = 0;
		break;
	}
}


struct cell_cluster *
life_cluster_new(int clusterX, int clusterY)
{
	struct cell_cluster *cluster;
	struct cell_cluster *neighbor;
	int clusteridx;
	int neighboridx;
	int neighborX, neighborY;
	int x, y;

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
	assert(clusteridx >= 0 && clusteridx < maxclusters);
	if (clustertable[clusteridx] != NULL)
		return (clustertable[clusteridx]);

	cluster = calloc(1, sizeof(*cluster));
	if (cluster == NULL)
		exit (1);

	clustertable[clusteridx] = cluster;
	numclusters++;
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
life_cluster_delete(struct cell_cluster *cluster)
{
	struct cell_cluster *neighbor;
	int clusteridx;
	int neighboridx;

	assert(cluster->numcells == 0);
	assert(numclusters > 0);

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

	clusteridx = (cluster->clusterY * cluster_numX) + cluster->clusterX;
	clustertable[clusteridx] = NULL;
	free(cluster);
	numclusters--;
}


static __inline
void
life_cluster_draw(Display *dpy, Window window, struct cell_cluster *cluster,
		  int xstart, int ystart)
{
	XColor *drawcolor;
	int xoffset, yoffset;
	int cellX, cellY;
	int cellidx;

	cellidx = 0;
	for (cellY = 0, yoffset = ystart;
	     cellY < CLUSTERSIZE;
	     cellY++, yoffset += cellsize + CELLPAD) {

		for (cellX = 0, xoffset = xstart;
		     cellX < CLUSTERSIZE;
		     cellX++, xoffset += cellsize + CELLPAD) {

			cell c = cluster->cell[cellY][cellX];

			if (c == CELL_DEAD) {
				if (!leavetrails)
					continue;
				c = cluster->oldcell[cellY][cellX];
				if (c == CELL_DEAD)
					continue;
				drawcolor = &trailcolors[c];
			} else {
				/* Live cell. */
				drawcolor = &colors[c];
			}

			XSetForeground(dpy, gc_draw, drawcolor->pixel);
			XFillRectangle(dpy, buf, gc_draw, xoffset, yoffset,
				       cellsize, cellsize);
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
life_cell_set(int x, int y, int color)
{
	struct cell_cluster *cluster;
	int clusterX, clusterY;

	/*
	 * First, handle wrapping of the X and Y coordinates.
	 */
	while (x < 0)
		x += cell_numX;
	while (x >= cell_numX)
		x -= cell_numX;
	while (y < 0)
		y += cell_numY;
	while (y >= cell_numY)
		y -= cell_numY;

	/*
	 * Handle wrapping of the color coordinate also.
	 * Note that this routine cannot be called to kill cells.
	 */
	while (color <= CELL_MINALIVE)
		color += numcolors;
	while (color >= numcolors)
		color -= numcolors;

	/* Now convert into <cluster, cell> coordinates. */
	clusterX = x / CLUSTERSIZE;
	clusterY = y / CLUSTERSIZE;
	x = x % CLUSTERSIZE;
	y = y % CLUSTERSIZE;

	cluster = life_cluster_new(clusterX, clusterY);

	if (cluster->cell[y][x] != CELL_DEAD) {
		/* Already a cell there.  Let's merge them. */
		cluster->cell[y][x] = CELL_MINALIVE +
			((cluster->cell[y][x] - CELL_MINALIVE + color) / 2);
		return;
	}

	cluster->cell[y][x] = color + CELL_MINALIVE;
	cluster->numcells++;
	numcells++;

	if (x == 0 || y == 0 || x == CLUSTERSIZE - 1 || y == CLUSTERSIZE - 1)
		life_cluster_wakeneighbors(cluster);
}


void
life_random_pattern(void)
{
	struct cell_cluster *cluster;
	int cellX, cellY;
	int color;
	int clusterX, clusterY;
	int clusteridx;
	int tries;

	/*
	 * First, find an empty cluster.
	 * We don't strictly need an empty cluster, but finding one is a good
	 * sign of a fairly sparsly populated region of the screen.
	 */
	for (tries = 5; tries > 0; tries--) {
		clusteridx = random() % maxclusters;
		cluster = clustertable[clusteridx];
		if (cluster == NULL)
			break;
	}

	/* Didn't find an empty cluster.  Hope for better luck next time... */
	if (tries == 0)
		return;

	clusterY = clusteridx / cluster_numX;
	clusterX = clusteridx % cluster_numX;
	cellY = (clusterY * CLUSTERSIZE) + (random() % CLUSTERSIZE);
	cellX = (clusterX * CLUSTERSIZE) + (random() % CLUSTERSIZE);

	/*
	 * Write pattern.
	 * XXX I would love to be able to load random patterns from file since
	 *     there are so many great patterns out there.  But the parser for
	 *     common formats are pretty complicated lest they be hackish
	 *     (making this source file uglier than it is now).
	 */

	color = random() % numcolors;

	switch (random() % 4) {
	case 0:
		/* Rabbits pattern. */
		life_cell_set(cellX,     cellY,     color + random() % 8);
		life_cell_set(cellX + 4, cellY,     color + random() % 8);
		life_cell_set(cellX + 5, cellY,     color + random() % 8);
		life_cell_set(cellX + 6, cellY,     color + random() % 8);
		life_cell_set(cellX,     cellY + 1, color + random() % 8);
		life_cell_set(cellX + 1, cellY + 1, color + random() % 8);
		life_cell_set(cellX + 2, cellY + 1, color + random() % 8);
		life_cell_set(cellX + 5, cellY + 1, color + random() % 8);
		life_cell_set(cellX + 1, cellY + 2, color + random() % 8);
		break;

	case 1:
		/* B-heptomino pattern. */
		life_cell_set(cellX + 1, cellY,     color + random() % 8);
		life_cell_set(cellX,     cellY + 1, color + random() % 8);
		life_cell_set(cellX + 1, cellY + 1, color + random() % 8);
		life_cell_set(cellX + 2, cellY + 1, color + random() % 8);

		life_cell_set(cellX,     cellY + 2, color + random() % 8);
		life_cell_set(cellX + 2, cellY + 2, color + random() % 8);
		life_cell_set(cellX + 3, cellY + 2, color + random() % 8);
		break;

	case 2:
		/* Simple glider; good for cleaning up. */
		life_cell_set(cellX,     cellY,     color + random() % 8);
		life_cell_set(cellX + 1, cellY,     color + random() % 8);
		life_cell_set(cellX + 2, cellY,     color + random() % 8);
		life_cell_set(cellX,     cellY + 1, color + random() % 8);
		life_cell_set(cellX + 1, cellY + 2, color + random() % 8);
		break;

	case 3:
		/* Another glider, different orientation. */
		life_cell_set(cellX + 2, cellY,     color + random() % 8);
		life_cell_set(cellX + 1, cellY,     color + random() % 8);
		life_cell_set(cellX,     cellY,     color + random() % 8);
		life_cell_set(cellX,     cellY + 1, color + random() % 8);
		life_cell_set(cellX + 1, cellY + 2, color + random() % 8);
		break;

	default:
		assert(0);
		/* NOTREACHED */
	}
}


void
life_display_init(Display *dpy, Window window)
{
	XGCValues gcv;
	int coloridx;

	delay = get_integer_resource("delay", "Integer");
	if (delay < 0)
		delay = 0;

	numcolors = get_integer_resource("ncolors", "Integer");
	if (numcolors < 2)
		numcolors = 2;
	if (numcolors > CELL_MAXALIVE)
		numcolors = CELL_MAXALIVE;

	leavetrails = get_boolean_resource("trails", "Boolean");
	if (numcolors == 2)
		leavetrails = False;

	double_buffer = get_boolean_resource("doubleBuffer", "Boolean");
	DBEclear = get_boolean_resource("useDBEClear", "Boolean");

	XGetWindowAttributes(dpy, window, &xgwa);

	coloridx = random() % 360;
	if (leavetrails) {
		/*
		 * Create a dark color gradient to use for the trails.  The
		 * parameters to make_color_loop() must match those for the
		 * main color loop below.
		 * Note that in both maps we leave index 0 as NULL.  We use
		 * index 0 as a dead-cell identifier so a) we want to catch
		 * references to it as a color and b) otherwise we would waste
		 * an allocated color.
		 */
		numcolors /= 2;		/* Half the colors for trails. */
		if (numcolors > CELL_MAXALIVE - CELL_MINALIVE)
			numcolors = CELL_MAXALIVE - CELL_MINALIVE;
		trailcolors = calloc(sizeof(XColor), numcolors + CELL_MINALIVE);
		make_color_loop(dpy, xgwa.colormap,
				coloridx,		1,	0.4,
				(coloridx + 120) % 360,	1,	0.4,
				(coloridx + 240) % 360,	1,	0.4,
				&trailcolors[CELL_MINALIVE], &numcolors,
				True	/* allocate */,
				False	/* writable */);
	}

	if (numcolors > CELL_MAXALIVE - CELL_MINALIVE)
		numcolors = CELL_MAXALIVE - CELL_MINALIVE;

	/* Main color gradient: used for drawing live cells. */
	colors = calloc(sizeof(XColor), numcolors + CELL_MINALIVE);
	make_color_loop(dpy, xgwa.colormap,
			coloridx,		1,	1,
			(coloridx + 120) % 360,	1,	1,
			(coloridx + 240) % 360,	1,	1,
			&colors[CELL_MINALIVE], &numcolors,
			True	/* allocate */,
			False	/* writable */);

	/*
	 * XXX Should handle case where number of trail colors allocated
	 *     doesn't match the number of live cell colors allocated.
	 */

	if (double_buffer) {
#ifdef HAVE_DOUBLE_BUFFER_EXTENSION
		backbuf = xdbe_get_backbuffer(dpy, window,
				DBEclear ? XdbeBackground: XdbeUndefined);
		buf = backbuf;
#endif /* HAVE_DOUBLE_BUFFER_EXTENSION */

		/*
		 * If we failed to get a back buffer from the double-buffer
		 * extension, or if the extension is unavailable, fall back
		 * to software double-buffering.
		 */
		if (buf == NULL) {
			bufpage[0] = XCreatePixmap(dpy, window,
					xgwa.width, xgwa.height, xgwa.depth);
			bufpage[1] = XCreatePixmap(dpy, window,
					xgwa.width, xgwa.height, xgwa.depth);
			buf = bufpage[0];
		}
	} else {
		/* If not double buffering, just draw directly to the window. */
		buf = window;
	}

	gcv.foreground = get_pixel_resource("background", "Background",
					    dpy, xgwa.colormap);
	gc_erase = XCreateGC(dpy, buf, GCForeground, &gcv);

	gcv.background = gcv.foreground;
	gcv.foreground = get_pixel_resource("foreground", "Foreground",
					    dpy, xgwa.colormap);
	gc_draw = XCreateGC(dpy, buf, GCForeground|GCBackground, &gcv);

	if (bufpage[0] != NULL) {
		XFillRectangle(dpy, bufpage[0], gc_erase, 0, 0,
		    xgwa.width, xgwa.height);
	}
	if (bufpage[1] != NULL) {
		XFillRectangle(dpy, bufpage[0], gc_erase, 0, 0,
		    xgwa.width, xgwa.height);
	}

	/*
	 * Precompute variables used in life_display_update() which do not
	 * change during execution.
	 */
	Xclear = !DBEclear && !leavetrails;
#ifdef HAVE_DOUBLE_BUFFER_EXTENSION
	Xclear = Xclear || (backbuf == NULL);
	if (backbuf != NULL) {
		swapinfo.swap_window = window;
		if (leavetrails)
			swapinfo.swap_action = XdbeCopied;
		else if (DBEclear)
			swapinfo.swap_action = XdbeBackground;
		else
			swapinfo.swap_action = XdbeUndefined;
	}
#endif
}


void
life_display_update(Display *dpy, Window window)
{
	struct cell_cluster *cluster;
	int clusterX, clusterY;
	int clusteridx;
	int xoffset, yoffset;

	/*
	 * Clear current draw buffer.
	 */
	if (Xclear) {
		XFillRectangle(dpy, buf, gc_erase, 0, 0,
			       xgwa.width, xgwa.height);
	}

	/*
	 * Draw cells.
	 */
	clusteridx = 0;
	for (clusterY = 0, yoffset = display_offsetY;
	     clusterY < cluster_numY;
	     clusterY++, yoffset += (cellsize + CELLPAD) * CLUSTERSIZE) {

		for (clusterX = 0, xoffset = display_offsetX;
		     clusterX < cluster_numX;
		     clusterX++, xoffset += (cellsize + CELLPAD) * CLUSTERSIZE,
				 clusteridx++) {

			cluster = clustertable[clusteridx];
			if (cluster == NULL)
				continue;

			if (cluster->numcells != 0 || leavetrails) {
				life_cluster_draw(dpy, window, cluster,
						  xoffset, yoffset);
			}

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
	if (backbuf != NULL) {
		XdbeSwapBuffers(dpy, &swapinfo, 1);
		return;
	}
#endif
	if (double_buffer) {
		/* We are doing software double-buffering. */
		XCopyArea(dpy, buf, window, gc_erase, 0, 0,
			  xgwa.width, xgwa.height, 0, 0);
		buf = (buf == bufpage[0] ? bufpage[1] : bufpage[0]);
	}
}


char *progclass = "Life";

char *defaults [] = {
	".background:		black",
	".foreground:		white",
	"*delay:		50000",
	"*ncolors:		100",
	"*cellsize:		4",
	"*trails:		True",
	"*doubleBuffer:		True",
#ifdef HAVE_DOUBLE_BUFFER_EXTENSION
	"*useDBE:		True",
	"*useDBEClear:		True",
#endif /* HAVE_DOUBLE_BUFFER_EXTENSION */
	NULL
};

XrmOptionDescRec options [] = {
	{ "-delay",		".delay",	XrmoptionSepArg, NULL },
	{ "-ncolors",		".ncolors",	XrmoptionSepArg, NULL },
	{ "-cellsize",		".cellsize",	XrmoptionSepArg, NULL },
	{ "-trails",		".trails",	XrmoptionNoArg, "True" },
	{ "-no-trails",		".trails",	XrmoptionNoArg, "False" },
	{ "-db",		".doubleBuffer", XrmoptionNoArg, "True" },
	{ "-no-db",		".doubleBuffer", XrmoptionNoArg, "False" },
	{ NULL, NULL, NULL, NULL }
};

void
screenhack (Display *dpy, Window window)
{
	life_display_init(dpy, window);
	life_state_init();

#if 0
{ /* XXXX */
	int i;
	int s = CLUSTERSIZE * (cellsize + 1);
	XSetForeground(dpy, gc_draw, colors[numcolors].pixel);
	for (i = 1; i < cluster_numY; i++) {
		XDrawLine(dpy, buf, gc_draw,
			  0, (i * s) + 1,
			  xgwa.width, (i * s) + 1);
	}
	for (i = 1; i < cluster_numX; i++) {
		XDrawLine(dpy, buf, gc_draw,
			  (i * s) + 1, 0,
			  (i * s) + 1, xgwa.height);
	}
}
#endif

	for (;;) {
		life_display_update(dpy, window);
		life_state_update();
		XSync(dpy, False);

		screenhack_handle_events(dpy);
		if (delay)
			usleep(delay);
	}
}
