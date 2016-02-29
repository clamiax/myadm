/* cc -D_BSD_SOURCE -std=c99 -O0 -Wall -pedantic -o core core.c $(mysql_config --cflags) -lmysqlclient -lstfl -lncursesw */
/* http://www.chiark.greenend.org.uk/~sgtatham/algorithms/listsort.html */
/* What about don't allocate item->fields in mysql_items() but just assign it to
 * row[i], then only free res instead of all items? */
/* selitem dereferences a NULL pointer if the previous view gets destroyed */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#include <mysql.h>
#include <stfl.h>

#include <langinfo.h>
#include <locale.h>

#define LENGTH(X)               (sizeof X / sizeof X[0])

typedef union {
	int i;
	unsigned int ui;
	float f;
	const void *v;
} Arg;

typedef struct Item Item;
struct Item {
	char **fields;
	int nfields;
	unsigned int flags;
	Item *next;
};

typedef struct {
	const char *mode;
	const wchar_t *modkey;
	void (*func)(const Arg *);
	const Arg arg;
} Key;

typedef struct {
	const char *name;
	void (*func)(void);
} Mode;

typedef struct View View;
struct View {
	Mode *mode;
	Item *items;
	int nitems;
	struct stfl_form *form;
	View *next;
};

/* function declarations */
void die(const char *errstr, ...);
void status(const char *fmtstr, ...);
void databases(void);
void tables(void);
void records(void);
void text(void);
MYSQL_RES *mysql_exec(char *sql);
int mysql_items(MYSQL_RES *res, Item **items);
void attach(View *v);
void detach(View *v);
void attachitemto(Item *i, Item **ii);
void detachitemfrom(Item *i, Item **ii);
void *ecalloc(size_t nmemb, size_t size);
void cleanupview(View *v);
void cleanupitems(Item *i);
Item *getitem(void);
void flagas(const Arg *arg);
void apply(const Arg *arg);
void quit(const Arg *arg);
void setmode(const Arg *arg);
void viewprev(const Arg *arg);
void usedb(const Arg *arg);
void usetable(const Arg *arg);
void userecord(const Arg *arg);
void itempos(const Arg *arg);

/* config.h > */

/* XXX command line arguments */
#define DBHOST "localhost"
#define DBUSER "root"
#define DBPASS "m0r3s3cur3"

static Mode modes[] = {
	/* name         show function */
	{ "databases",  databases }, /* first entry is default */
	{ "tables",     tables },
	{ "records",    records },
	{ "text",       text },
};

static Key keys[] = {
	/* mode          modkey        function        argument */
        { NULL,          L"Q",         quit,           {.i = 1} },
        { NULL,          L"q",         viewprev,       {0} },
        { NULL,          L"D",         setmode,        {.v = &modes[0]} },
        { NULL,          L"T",         setmode,        {.v = &modes[1]} },
        { NULL,          L"R",         setmode,        {.v = &modes[2]} },
        { NULL,          L"E",         setmode,        {.v = &modes[3]} },
        { NULL,          L"k",         itempos,        {.i = -1} },
        { NULL,          L"j",         itempos,        {.i = +1} },
        { "databases",   L"q",         quit,           {.i = 0} },
        { "databases",   L"ENTER",     usedb,          {.v = &modes[1]} },
        { "databases",   L"SPACE",     usedb,          {.v = &modes[1]} },
        { "tables",      L"ENTER",     usetable,       {.v = &modes[2]} },
        { "tables",      L"SPACE",     usetable,       {.v = &modes[2]} },
        { "records",     L"ENTER",     userecord,      {0} },
        { "records",     L"d",         flagas,         {.v = "D"} },
        { "records",     L"t",         flagas,         {.v = "*"} },
        { "records",     L"$",         apply,          {0} },
};

/* < config.h */

/* variables */
static int running = 1;
static MYSQL *mysql;
static View *views, *selview;
static struct stfl_ipool *ipool;
static Item *selitem;

/* function implementations */
void
quit(const Arg *arg) {
	char c;

	if(arg->i) {
		status("Do you want to quit ([y]/n)?");
		stfl_run(selview->form, -1);
		while((c = getchar())) {
			if(c == 'n') {
				status("");
				return;
			}
			if(c == 'y' || c == '\r')
				break;
		}
	}
	running = 0;
}

void
attach(View *v) {
	v->next = views;
	views = v;
}

void
detach(View *v) {
	View **tv;

	for (tv = &views; *tv && *tv != v; tv = &(*tv)->next);
	*tv = v->next;
}

/* XXX attachto(void, void)? */
void
attachitemto(Item *i, Item **ii) {
	i->next = *ii;
	*ii = i;
}

void
detachitemfrom(Item *i, Item **ii) {
	Item **ti;

	for (ti = &(*ii); *ti && *ti != i; ti = &(*ti)->next);
	*ti = i->next;
}

void
setmode(const Arg *arg) {
	const Mode *m = arg->v;
	View *v;
	unsigned int i;

	if(selview && !strcmp(arg->v, m->name))
		return;
	for(v = views; v; v = v->next)
		if(!strcmp(v->mode->name, m->name))
			break;
	if(!v) {
		v = ecalloc(1, sizeof(View));
		for(i = 0; i < LENGTH(modes); ++i)
			if(!strcmp(modes[i].name, m->name))
				v->mode = &modes[i];
		attach(v);
	}

	selview = v;
	v->mode->func();
}

void
cleanupitems(Item *i) {
	while(i && i->next) {
		detachitemfrom(i, &i);
		while(--i->nfields >= 0)
			free(i->fields[i->nfields]);
		free(i->fields);
		free(i);
	}
}

void
cleanupview(View *v) {
	detach(v);
	cleanupitems(v->items);
	if(v->form)
		stfl_free(v->form);
	free(v);
}

MYSQL_RES *
mysql_exec(char *sql) {
	MYSQL_RES *res;

	if(mysql_real_query(mysql, sql, strlen(sql)))
		return NULL;
	res = mysql_store_result(mysql);
	if(!res)
		return NULL; /* XXX if(mysql_field_count(mysql)) error; */
	return res;
}

int
mysql_items(MYSQL_RES *res, Item **items) {
	MYSQL_ROW row;
	Item *item;
	int i, nfds, nrows;

	nfds = mysql_num_fields(res);
	nrows = mysql_num_rows(res);

	*items = NULL;
	while((row = mysql_fetch_row(res))) {
		item = ecalloc(1, sizeof(Item));
		item->nfields = nfds; /* XXX this should go into the view */
		item->fields = ecalloc(nfds, sizeof(char *));
		for(i = 0; i < nfds; ++i) {
			item->fields[i] = ecalloc(32, sizeof(char));
			snprintf(item->fields[i], 32, "%s", row[i]);
		}

		attachitemto(item, &(*items));
	}
	return nrows;
}

void
databases(void) {
	MYSQL_RES *res;
	Item *item;
	char txt[256];

	if(!(res = mysql_exec("show databases")))
		die("databases");

	cleanupitems(selview->items);
	selview->nitems = mysql_items(res, &selview->items);
	mysql_free_result(res);

	if(!selview->form)
		selview->form = stfl_create(L"<items.stfl>");

	stfl_modify(selview->form, L"items", L"replace_inner", L"vbox"); /* clear */
	for(item = selview->items; item; item = item->next) {
		snprintf(txt, sizeof txt, "listitem text:\"%s\"", item->fields[0]);
		stfl_modify(selview->form, L"items", L"append", stfl_ipool_towc(ipool, txt));
	}
	stfl_set(selview->form, L"pos", 0);
}

void
tables(void) {
	MYSQL_RES *res;
	Item *item;
	char txt[256];

	if(!(res = mysql_exec("show tables")))
		die("tables\n");

	cleanupitems(selview->items);
	selview->nitems = mysql_items(res, &selview->items);
	mysql_free_result(res);

	if(!selview->form)
		selview->form = stfl_create(L"<items.stfl>");

	stfl_modify(selview->form, L"items", L"replace_inner", L"vbox"); /* clear */
	for(item = selview->items; item; item = item->next) {
		snprintf(txt, sizeof txt, "listitem text:\"%s\"", item->fields[0]);
		stfl_modify(selview->form, L"items", L"append", stfl_ipool_towc(ipool, txt));
	}
	stfl_set(selview->form, L"pos", 0);
}

void
records(void) {
	Item *item;
	MYSQL_RES *res;
	char txt[512];
	char t[32];

	snprintf(txt, sizeof txt, "select * from `%s`", selitem->fields[0]);
	if(!(res = mysql_exec(txt)))
		die("tables\n");

	cleanupitems(selview->items);
	selview->nitems = mysql_items(res, &selview->items);
	mysql_free_result(res);

	if(!selview->form)
		selview->form = stfl_create(L"<items.stfl>");

	stfl_modify(selview->form, L"items", L"replace_inner", L"vbox"); /* clear */
	for(item = selview->items; item; item = item->next) {
		strncpy(txt, "listitem text:\"", sizeof txt);
		for(int i = 0; i < item->nfields; ++i) {
			snprintf(t, sizeof t, "%8.16s", item->fields[i]);
			if(i)
				strncat(txt, " | ", sizeof txt);
			/* XXX What if strlen(all_fields) > sizeof txt? */
			strncat(txt, t, sizeof txt);
		}
		strncat(txt, "\"", sizeof txt);
		stfl_modify(selview->form, L"items", L"append", stfl_ipool_towc(ipool, txt));
	}
	stfl_set(selview->form, L"pos", 0);
}

void
text(void) {
}

Item *
getitem(void) {
	int pos = atoi(stfl_ipool_fromwc(ipool, stfl_get(selview->form, L"pos")));
	int i = 0;
	Item *item = NULL;

	for(item = selview->items; item; item = item->next)
		if(i++ == pos)
			break;
	return item;
}

void
usedb(const Arg *arg) {
	Item *item = getitem();
	mysql_select_db(mysql, item->fields[0]);
	setmode(arg);
}

void
usetable(const Arg *arg) {
	selitem = getitem();
	setmode(arg);
}

void
userecord(const Arg *arg) {
}

void
itempos(const Arg *arg) {
	int pos = atoi(stfl_ipool_fromwc(ipool, stfl_get(selview->form, L"pos")));
	char tmp[8];

	pos += arg->i;
	if(pos < 0)
		pos = 0;
	else if(pos >= selview->nitems)
		pos = selview->nitems - 1;

	snprintf(tmp, sizeof tmp, "%d", pos);
	stfl_set(selview->form, L"pos", stfl_ipool_towc(ipool, tmp));
}

void
viewprev(const Arg *arg) {
	if(!selview->next)
		return;
	selview = selview->next;
	selview->mode->func();
}

void
flagas(const Arg *arg) {
}

void
apply(const Arg *arg) {
}

void
status(const char *fmtstr, ...) {
	/*
	va_start(ap, errstr);
	vfprintf(stderr, errstr, ap);
	va_end(ap);
	*/

	stfl_set(selview->form, L"stext", stfl_ipool_towc(ipool, fmtstr));
}

void
die(const char *errstr, ...) {
	va_list ap;

	va_start(ap, errstr);
	vfprintf(stderr, errstr, ap);
	va_end(ap);
	exit(1);
}

void *
ecalloc(size_t nmemb, size_t size) {
	void *p;

	if (!(p = calloc(nmemb, size)))
		die("Cannot allocate memory.");
	return p;
}

int
main(int argc, char **argv) {
	Arg a = {.v = &modes[0]};
	Key *k;
	const wchar_t *ev;
	unsigned int i;

	mysql = mysql_init(NULL);
	if(mysql_real_connect(mysql, DBHOST, DBUSER, DBPASS, NULL, 0, NULL, 0) == NULL)
		die("Cannot connect to the database.\n");

	ipool = stfl_ipool_create(nl_langinfo(CODESET));
	setmode(&a);
	status("Welcome to core.c");

	while(running) {
		if(!(ev = stfl_run(selview->form, 0)))
			continue;
		status("");
		k = NULL;
		for(i = 0; i < LENGTH(keys); ++i)
			if(!((keys[i].mode && strcmp(selview->mode->name, keys[i].mode))
			|| wcscmp(ev, keys[i].modkey)))
				k = &keys[i];
		if(k)
			k->func(&k->arg);
	}

	while(views)
		cleanupview(views);

	stfl_reset();
	stfl_ipool_destroy(ipool);
	return 0;
}
