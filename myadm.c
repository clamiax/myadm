/* See LICENSE file for copyright and license details.
 *
 * myadm is a text-based TUI for MySQL. It emulates the mutt interface through
 * the STFL library and talk with the SQL server using libmysqlclient.
 *
 * Each piece of information displayed is called an item. Items are organized
 * in a linked items list on each view. A view contains an STFL form where all
 * graphical elements are drawn along with all related informations. Each item
 * contains a bit array to indicate tags of an item.
 *
 * To understand everything else, start reading main().
*/

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <ctype.h>
#include <mysql.h>
#include <stfl.h>
#include <langinfo.h>
#include <locale.h>
#include <curses.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "arg.h"
char *argv0;

#define QUOTE(S)		(stfl_ipool_fromwc(ipool, stfl_quote(stfl_ipool_towc(ipool, S))))
#define ISCURMODE(N)		!(N && selview && strcmp(selview->mode.name, N))
#define LENGTH(X)               (sizeof X / sizeof X[0])

#define MYSQLIDLEN		64
#define MAXQUERYLEN		4096

typedef union {
	int i;
	unsigned int ui;
	float f;
	const void *v;
} Arg;

typedef struct Item Item;
struct Item {
	char **cols;
	int *lens;
	int ncols;
	int id;
	Item *next;
};

typedef struct Field Field;
struct Field {
	char name[MYSQLIDLEN];
	int len;
	Field *next;
};

typedef struct {
	const char *mode;
	const int modkey;
	void (*func)(const Arg *);
	const Arg arg;
} Key;

typedef struct {
	char name[16];
	void (*func)(void);
} Mode;

typedef struct View View;
struct View {
	Mode mode;
	Item *items;
	Item *choice;
	Field *fields;
	int cur;
	int nitems;
	int nfields;
	struct stfl_form *form;
	View *next;
};

/* function declarations */
void attach(View *v);
void attachfield(Field *f, Field **ff);
void attachitem(Item *i, Item **ii);
char ui_ask(const char *msg, char *opts);
void cleanup(void);
void cleanupfields(Field **fields);
void cleanupitems(Item **items);
void cleanupview(View *v);
void detach(View *v);
void detachfield(Field *f, Field **ff);
void detachitem(Item *i, Item **ii);
void die(const char *errstr, ...);
void *ecalloc(size_t nmemb, size_t size);
void editfile(char *file);
void editrecord(const Arg *arg);
int escape(char *esc, char *s, int sz, char c, char q);
Item *getitem(int pos);
int *getmaxlengths(Item *items, Field *fields);
void itemsel(const Arg *arg);
char *mksql_update_record(Item *item, Field *fields, char *tbl, char *pk);
int mysql_file_exec(char *file);
int mysql_exec(const char *sqlstr, ...);
int mysql_fields(MYSQL_RES *res, Field **fields);
void mysql_fillview(MYSQL_RES *res, int showfds);
int mysql_pkey(char *key, char *tbl);
int mysql_items(MYSQL_RES *res, Item **items);
View *newaview(const char *name, void (*func)(void));
void quit(const Arg *arg);
void reload(const Arg *arg);
void run(void);
void setview(const char *name, void (*func)(void));
void setup(void);
void ui_end(void);
struct stfl_form *ui_getform(wchar_t *code);
void ui_init(void);
void ui_modify(const char *name, const char *mode, const char *fmtstr, ...);
void ui_listview(Item *items, Field *fields);
void ui_putitem(Item *item, int *lens);
void ui_redraw(void);
void ui_refresh(void);
void ui_set(const char *key, const char *fmtstr, ...);
void ui_showfields(Field *fds, int *lens);
void ui_showitems(Item *items, int *lens);
void ui_sql_edit_exec(char *sql);
void usage(void);
void viewdb(const Arg *arg);
void viewdb_show(void);
void viewdblist_show(void);
void viewprev(const Arg *arg);
void viewtable(const Arg *arg);
void viewtable_show(void);

#include "config.h"

/* variables */
static int running = 1;
static MYSQL *mysql;
static View *views, *selview = NULL;
static struct stfl_ipool *ipool;
static int fldseplen;

/* function implementations */
void
attach(View *v) {
	v->next = views;
	views = v;
}

void
attachfield(Field *f, Field **ff) {
	Field **l;

	for(l = ff; *l && (*l)->next; l = &(*l)->next);
	if(!*l)
		*l = f;
	else
		(*l)->next = f;
}

void
attachitem(Item *i, Item **ii) {
	Item **l;

	for(l = ii; *l && (*l)->next; l = &(*l)->next);
	if(!*l)
		*l = i;
	else
		(*l)->next = i;
}

char
ui_ask(const char *msg, char *opts) {
	int c;
	char *o = NULL;

	ui_set("status", msg);
	ui_refresh();
	while((c = getch())) {
		if(c == '\n') {
			o = &opts[0];
			break;
		}
		for(o = opts; *o; ++o)
			if(c == *o)
				break;
		if(*o)
			break;
	}
	ui_set("status", "");
	return *o;
}

void
cleanup(void) {
	while(views)
		cleanupview(views);
	ui_end();
	mysql_close(mysql);
}

void
cleanupview(View *v) {
	detach(v);
	cleanupitems(&v->items);
	cleanupfields(&v->fields);
	if(v->form)
		stfl_free(v->form);
	free(v);
}

void
cleanupfields(Field **fields) {
	Field *f;

	while(*fields) {
		f = *fields;
		detachfield(f, fields);
		free(f);
	}
}

void
cleanupitems(Item **items) {
	Item *i;

	while(*items) {
		i = *items;
		detachitem(i, items);
		while(--i->ncols >= 0)
			free(i->cols[i->ncols]);
		free(i->lens);
		free(i->cols);
		free(i);
	}
}

void
detach(View *v) {
	View **tv;

	for(tv = &views; *tv && *tv != v; tv = &(*tv)->next);
	*tv = v->next;
}

void
detachfield(Field *f, Field **ff) {
	Field **tf;

	for(tf = &(*ff); *tf && *tf != f; tf = &(*tf)->next);
	*tf = f->next;
}

void
detachitem(Item *i, Item **ii) {
	Item **ti;

	for(ti = &(*ii); *ti && *ti != i; ti = &(*ti)->next);
	*ti = i->next;
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

void
editfile(char *file) {
        pid_t pid;
	int rc = -1;
	struct sigaction saold[4];

	/* take off ncurses signal handlers */
	struct sigaction sa = {.sa_flags = 0, .sa_handler = SIG_DFL};
	sigaction(SIGINT, &sa, &saold[0]);
	sigaction(SIGTERM, &sa, &saold[1]);
	sigaction(SIGTSTP, &sa, &saold[2]);
	sigaction(SIGWINCH, &sa, &saold[3]);

        if((pid = fork()) == 0) {
                execl("/bin/sh", "sh", "-c", "$EDITOR \"$0\"", file, NULL);
                _exit(127);
        }
	else if(pid == -1)
		return;
	while(!WIFEXITED(rc))
		waitpid(pid, &rc, 0);

	/* restore ncurses signal handlers */
	sigaction(SIGINT, &saold[0], NULL);
	sigaction(SIGTERM, &saold[1], NULL);
	sigaction(SIGTSTP, &saold[2], NULL);
	sigaction(SIGWINCH, &saold[3], NULL);

	ui_redraw();
}

void
editrecord(const Arg *arg) {
	Item *item = getitem(0);
	char *tbl = selview->choice->cols[0], pk[MYSQLIDLEN+1], *sql;

	if(!item) {
		ui_set("status", "No item selected.");
		return;
	}
	if(mysql_pkey(pk, tbl)) {
		ui_set("status", "Cannot edit records in `%s`, no unique key found.", tbl);
		return;
	}
	sql = mksql_update_record(item, selview->fields, tbl, pk);
	ui_sql_edit_exec(sql);
	free(sql);
}

int
escape(char *esc, char *s, int sz, char c, char q) {
	int i, ei = 0;

	for(i = 0; i < sz; ++i) {
		if(s[i] == c && (!q || s[i+1] != q))
			esc[ei++] = '\\';
		esc[ei++] = s[i];
	}
	esc[ei] = '\0';
	return ei;
}

Item *
getitem(int pos) {
	Item *item;
	int n;

	if(!selview)
		return NULL;
	if(!pos)
		pos = selview->cur;
	for(item = selview->items, n = 0; item; item = item->next, ++n)
		if(n == pos)
			break;
	return item;
}

int *
getmaxlengths(Item *items, Field *fields) {
	Item *item;
	Field *fld;
	int i, *lens, ncols;

	if(!(items || fields))
		return NULL;

	if(items)
		ncols = items->ncols;
	else
		for(fld = fields, ncols = 0; fld; fld = fld->next, ++ncols);
	lens = ecalloc(ncols, sizeof(int));
	if(fields)
		for(fld = fields, i = 0; fld; fld = fld->next, ++i)
			lens[i] = (fld->len <= MAXCOLSZ ? fld->len : MAXCOLSZ);
	if(items)
		for(item = items; item; item = item->next)
			for(i = 0; i < item->ncols; ++i)
				if(lens[i] < item->lens[i])
					lens[i] = (item->lens[i] <= MAXCOLSZ ? item->lens[i] : MAXCOLSZ);
	return lens;
}

void
itemsel(const Arg *arg) {
	int pos;

	if(!selview)
		return;
	pos = selview->cur + arg->i;
	if(pos < 0)
		pos = 0;
	else if(pos >= selview->nitems)
		pos = selview->nitems - 1;
	ui_set("pos", "%d", pos);
	selview->cur = pos;
}

char *
mksql_update_record(Item *item, Field *fields, char *tbl, char *pk) {
	Field *fld;
	char *sql, *col = NULL, *sqlfds = NULL, *pkv = NULL;
	size_t i, len = 0, cnt = 0, size = 0, max = 0;

	for(i = 0, fld = fields; fld; fld = fld->next, ++i) {
		if(!pkv && !strncmp(pk, fld->name, fld->len))
			pkv = item->cols[i];
		len = 10 + fld->len;
		if(item->lens[i] > max) {
			max = item->lens[i];
			if(!(col = realloc(col, max*2+1)))
				die("cannot realloc %u bytes:", max*2+1);
		}
		len += escape(col, item->cols[i], item->lens[i], '\'', 0);
		if(cnt + len >= size)
			if(!(sqlfds = realloc(sqlfds, (size += (len <= BUFSIZ ? BUFSIZ : len)))))
				die("cannot realloc %u bytes:", size);
		snprintf(&sqlfds[cnt], len, "\n%c`%s` = '%s'",
			cnt ? ',' : ' ', fld->name, col);
		cnt += len - 1;
	}
	free(col);
	size = 1 + 28 + cnt + strlen(pk) + strlen(pkv) + strlen(tbl);
	sql = ecalloc(1, size);
	snprintf(sql, size, "UPDATE `%s` SET%s\nWHERE `%s` = '%s'", tbl, sqlfds, pk, pkv);
	free(sqlfds);
	return sql;
}

int
mysql_exec(const char *sqlstr, ...) {
	va_list ap;
	char sql[MAXQUERYLEN];
	int r, sqlen;

	va_start(ap, sqlstr);
	sqlen = vsnprintf(sql, sizeof sql, sqlstr, ap);
	va_end(ap);
	r = mysql_real_query(mysql, sql, sqlen);
	return (r ? -1 : mysql_field_count(mysql));
}

int
mysql_fields(MYSQL_RES *res, Field **fields) {
	MYSQL_FIELD *fds;
	Field *field;
	int nfds, i;

	nfds = mysql_num_fields(res);
	if(!nfds)
		return 0;
	fds = mysql_fetch_fields(res);
	for(i = 0; i < nfds; ++i) {
		field = ecalloc(1, sizeof(Field));
		field->len = fds[i].name_length;
		memcpy(field->name, fds[i].name, field->len);
		attachfield(field, fields);
	}
	return nfds;
}

int
mysql_file_exec(char *file) {
	char *buf, *esc;
	int fd, size, r;

	fd = open(file, O_RDONLY);
	if(fd == -1)
		return -1;
	lseek(fd, 0, SEEK_SET);
	size = lseek(fd, 0, SEEK_END);
	lseek(fd, 0, SEEK_SET);
	buf = ecalloc(1, size+1);
	if(read(fd, buf, size) != size) {
		free(buf);
		return -2;
	}
	buf[size] = '\0';

	/* We do not want flow control chars to be interpreted. */
	esc = ecalloc(1, size*2+1);
	escape(esc, buf, size, '\\', '\'');
	r = mysql_exec(esc);
	free(buf);
	free(esc);
	if(r == -1)
		return -3;
	return 0;
}

void
mysql_fillview(MYSQL_RES *res, int showfds) {
	cleanupitems(&selview->items);
	selview->nitems = mysql_items(res, &selview->items);
	if(showfds) {
		cleanupfields(&selview->fields);
		selview->nfields = mysql_fields(res, &selview->fields);
	}
}

int
mysql_pkey(char *key, char *tbl) {
	MYSQL_RES *res;
	MYSQL_ROW row;
	int r;

	r = mysql_exec("show keys from `%s` where Non_unique = 0", tbl);
	if(r == -1 || !(res = mysql_store_result(mysql)))
		return 1;
	if(!(row = mysql_fetch_row(res))) {
		mysql_free_result(res);
		return 2;
	}
	sprintf(key, "%s", row[4]);
	mysql_free_result(res);
	return 0;
}

int
mysql_items(MYSQL_RES *res, Item **items) {
	MYSQL_ROW row;
	Item *item;
	int id = 0 , i, nfds, nrows;
	unsigned long *lens;

	nfds = mysql_num_fields(res);
	nrows = mysql_num_rows(res);
	*items = NULL;
	while((row = mysql_fetch_row(res))) {
		item = ecalloc(1, sizeof(Item));
		item->lens = ecalloc(nfds, sizeof(int));
		item->cols = ecalloc(nfds, sizeof(char *));
		item->id = ++id;
		lens = mysql_fetch_lengths(res);
		item->ncols = nfds;
		for(i = 0; i < nfds; ++i) {
			item->cols[i] = ecalloc(lens[i], sizeof(char) + 1);
			memcpy(item->cols[i], row[i], lens[i]);
			item->lens[i] = lens[i];
		}
		attachitem(item, items);
	}
	return nrows;
}

void
ui_listview(Item *items, Field *fields) {
	int *lens;

	if(!selview->form)
		selview->form = ui_getform(L"<items.stfl>");
	lens = getmaxlengths(items, fields);
	if(fields)
		ui_showfields(fields, lens);
	if(items)
		ui_showitems(items, lens);
	free(lens);
}

void
ui_showfields(Field *fds, int *lens) {
	Field *fld;
	char line[COLS + 1];
	int li = 0, i, j;

	if(!(fds && lens))
		return;
	line[0] = '\0';
	for(fld = fds, i = 0; fld; fld = fld->next, ++i) {
		if(i) {
			for(j = 0; j < fldseplen && li < COLS; ++j)
				line[li++] = FLDSEP[j];
			if(li == COLS)
				break;
		}
		for(j = 0; j < fld->len && j < lens[i] && li < COLS; ++j)
			line[li++] = fld->name[j];
		while(j++ < lens[i] && li < COLS)
			line[li++] = ' ';
	}
	line[li] = '\0';
	ui_set("subtle", "%s", line);
	ui_set("showsubtle", "%d", (line[0] ? 1 : 0));
}

void
ui_showitems(Item *items, int *lens) {
	Item *item;

	ui_modify("items", "replace_inner", "vbox"); /* empty items */
	for(item = selview->items; item; item = item->next)
		ui_putitem(item, lens);
	ui_set("pos", 0);
}

void
ui_sql_edit_exec(char *sql) {
	struct stat sb, sa;
	int fd;
	char tmpf[] = "/tmp/myadm.XXXXXX", *yn = "yn";

        fd = mkstemp(tmpf);
	if(fd == -1) {
		ui_set("status", "Cannot make a temporary file.");
		return;
	}
        if(write(fd, sql, strlen(sql)) == -1) {
		close(fd);
		unlink(tmpf);
		ui_set("status", "Cannot write into the temporary file.");
		return;
	}
	close(fd);

	stat(tmpf, &sb);
	while(1) {
		editfile(tmpf);
		stat(tmpf, &sa);
		if(!sa.st_size || sb.st_mtime == sa.st_mtime) {
			ui_set("status", "No changes.");
			break;
		}
		if(mysql_file_exec(tmpf) < 0) {
			if(*mysql_error(mysql)) {
				if(ui_ask("Wrong SQL code. Continue editing ([y]/n)?", yn) == yn[0])
					continue;
				break;
			}
			ui_set("status", "Something went wrong."); /* XXX improve */
			break;
		}
		reload(NULL);
		ui_set("status", "Updated.");
		break;
	}
	unlink(tmpf);
}

View *
newaview(const char *name, void (*func)(void)) {
	View *v;

	v = ecalloc(1, sizeof(View));
	v->choice = getitem(0);
	strncpy(v->mode.name, name, sizeof v->mode.name);
	v->mode.func = func;
	attach(v);
	return v;
}

/* XXX Improved logic:
 * -1 only ask if there are pending changes
 *  1 always ask 
 *  0 never ask */
void
quit(const Arg *arg) {
	char *yn= "yn";

	if(arg->i)
		if(ui_ask("Do you want to quit ([y]/n)?", yn) != yn[0])
			return;
	running = 0;
}

void
reload(const Arg *arg) {
	char tmp[8];

	if(!selview->mode.func)
		return;
	selview->mode.func();
	if(selview->cur) {
		snprintf(tmp, sizeof tmp, "%d", selview->cur);
		ui_set("pos", tmp);
	}
}

void
run(void) {
	Key *k;
	int code;
	int i;

	while(running) {
		ui_refresh();
		code = getch();
		if(code < 0)
			continue;
		k = NULL;
		for(i = 0; i < LENGTH(keys); ++i)
			if(ISCURMODE(keys[i].mode) && keys[i].modkey == code) {
				k = &keys[i];
				break;
			}
		if(k) {
			ui_set("status", "");
			k->func(&k->arg);
		}
	}
}

void
setview(const char *name, void (*func)(void)) {
	selview = newaview(name, func);
	func();
}

void
setup(void) {
	setlocale(LC_CTYPE, "");
	mysql = mysql_init(NULL);
	if(mysql_real_connect(mysql, dbhost, dbuser, dbpass, NULL, 0, NULL, 0) == NULL)
		die("Cannot connect to the database.\n");
	fldseplen = strlen(FLDSEP);
	ui_init();
	setview("databases", viewdblist_show);
}

void
ui_end(void) {
	stfl_reset();
	stfl_ipool_destroy(ipool);
}

struct stfl_form *
ui_getform(wchar_t *code) {
	struct stfl_form *f;

	f = stfl_create(code);
	return f;
}

void
ui_init(void) {
	struct stfl_form *f = ui_getform(L"label");

	stfl_run(f, -3); /* init ncurses */
	stfl_free(f);
	nocbreak();
	raw();
	curs_set(0);
	ipool = stfl_ipool_create(nl_langinfo(CODESET));
}

void
ui_modify(const char *name, const char *mode, const char *fmtstr, ...) {
	va_list ap;
	char txt[1024];

	if(!selview->form)
		return;
	va_start(ap, fmtstr);
	vsnprintf(txt, sizeof txt, fmtstr, ap);
	va_end(ap);
	stfl_modify(selview->form,
		stfl_ipool_towc(ipool, name),
		stfl_ipool_towc(ipool, mode),
		stfl_ipool_towc(ipool, txt));
}

void
ui_putitem(Item *item, int *lens) {
	char line[COLS + 1];
	int pad, li = 0, i, j;

	if(!(item && lens))
		return;
	line[0] = '\0';
	for(i = 0; i < item->ncols; ++i) {
		if(i) {
			for(j = 0; j < fldseplen && li < COLS; ++j)
				line[li++] = FLDSEP[j];
			if(li == COLS)
				break;
		}
		pad = li;
		for(j = 0; j < item->lens[i] && j < lens[i] && li < COLS; ++j)
			line[li++] = (isprint(item->cols[i][j])
					? item->cols[i][j]
					: ' ');
		pad = li - pad;
		while(pad++ < lens[i] && li < COLS)
			line[li++] = ' ';
	}
	line[li] = '\0';
	ui_modify("items", "append", "listitem[%d] text:%s", item->id, QUOTE(line));
}

void
ui_redraw(void) {
	if(selview && selview->form)
		stfl_redraw(selview->form);
}

void
ui_refresh(void) {
	if(selview && selview->form)
		stfl_run(selview->form, -1);
}

void
ui_set(const char *key, const char *fmtstr, ...) {
	va_list ap;
	char val[256];

	if(!selview)
		return;

	va_start(ap, fmtstr);
	vsnprintf(val, sizeof val, fmtstr, ap);
	va_end(ap);
	stfl_set(selview->form, stfl_ipool_towc(ipool, key), stfl_ipool_towc(ipool, val));
}

void
usage(void) {
	die("Usage: %s [-vhup <arg>]\n", argv0);
}

void
viewdb(const Arg *arg) {
	Item *choice = getitem(0);

	if(!choice) {
		ui_set("status", "No database selected.");
		return;
	}
	mysql_select_db(mysql, choice->cols[0]);
	setview("tables", viewdb_show);
}

void
viewdb_show(void) {
	MYSQL_RES *res;

	if(mysql_exec("show tables") == -1 || !(res = mysql_store_result(mysql)))
		die("show tables");
	mysql_fillview(res, 0);
	mysql_free_result(res);
	ui_listview(selview->items, NULL);
	ui_set("title", "Tables in `%s`", selview->choice->cols[0]);
	ui_set("info", "%d table(s)", selview->nitems);
}

void
viewdblist_show(void) {
	MYSQL_RES *res;

	if(mysql_exec("show databases") == -1 || !(res = mysql_store_result(mysql)))
		die("show databases");
	mysql_fillview(res, 0);
	mysql_free_result(res);
	ui_listview(selview->items, NULL);
	ui_set("title", "Databases in `%s`", dbhost);
	ui_set("info", "%d DB(s)", selview->nitems);
}

void
viewprev(const Arg *arg) {
	View *v;

	if(!(selview && selview->next))
		return;
	v = selview->next;
	cleanupview(selview);
	selview = v;
}

void
viewtable(const Arg *arg) {
	if(!getitem(0)) {
		ui_set("status", "No table selected.");
		return;
	}
	setview("records", viewtable_show);
}

void
viewtable_show(void) {
	MYSQL_RES *res;
	Item *choice = selview->choice;
	int r;

	r = mysql_exec("select * from `%s`", choice->cols[0]);
	if(r == -1 || !(res = mysql_store_result(mysql)))
		die("select from `%s`", choice->cols[0]);
	mysql_fillview(res, 1);
	mysql_free_result(res);
	ui_listview(selview->items, selview->fields);
	ui_set("title", "Records in `%s`", choice->cols[0]);
	ui_set("info", "%d record(s)", selview->nitems);
}

int
main(int argc, char **argv) {
	ARGBEGIN {
	case 'h':
		dbhost = EARGF(usage());
		break;
	case 'u':
		dbuser = EARGF(usage());
		break;
	case 'p':
		dbpass = EARGF(usage());
		break;
	case 'v':
		die("%s " VERSION " (c) 2016 Claudio Alessi\n", argv0);
	default:
		usage();
	} ARGEND;

	setup();
	run();
	cleanup();
	return 0;
}
