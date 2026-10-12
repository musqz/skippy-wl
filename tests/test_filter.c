/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <string.h>

#include "filter.h"

static int failures;

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		failures++; \
	} \
} while (0)

static struct filter make(const char *app_id, const char *title, const char *status)
{
	struct filter f;

	memset(&f, 0, sizeof(f));
	if (app_id)
		CHECK(filter_set(&f, "app-id", app_id) == 0);
	if (title)
		CHECK(filter_set(&f, "title", title) == 0);
	if (status)
		CHECK(filter_set(&f, "status", status) == 0);
	return f;
}

int main(void)
{
	struct filter f = make(NULL, NULL, NULL);

	/* empty filter matches everything, even a missing app_id */
	CHECK(filter_match(&f, "kitty", "vim", 0));
	CHECK(filter_match(&f, NULL, NULL, STATUS_MINIMIZED));

	f = make("kitty", NULL, NULL);
	CHECK(filter_match(&f, "kitty", "x", 0));
	CHECK(!filter_match(&f, "kitty2", "x", 0));
	CHECK(filter_match(&f, "KITTY", "x", 0));
	CHECK(!filter_match(&f, NULL, "x", 0));

	f = make("kitty,foot", NULL, NULL);
	CHECK(filter_match(&f, "foot", "", 0) && filter_match(&f, "kitty", "", 0));
	CHECK(!filter_match(&f, "firefox", "", 0));

	f = make("!kitty", NULL, NULL);
	CHECK(!filter_match(&f, "kitty", "", 0) && filter_match(&f, "foot", "", 0));

	/* plain and negated terms together: any plain term, no negated term */
	f = make("org.*,!org.gnome.*", NULL, NULL);
	CHECK(filter_match(&f, "org.kde.dolphin", "", 0));
	CHECK(!filter_match(&f, "org.gnome.Nautilus", "", 0));
	CHECK(!filter_match(&f, "kitty", "", 0));

	f = make(NULL, "*vim*", NULL);
	CHECK(filter_match(&f, "kitty", "nvim README.md", 0));
	CHECK(!filter_match(&f, "kitty", "bash", 0));

	/* both fields must match */
	f = make("kitty", "*vim*", NULL);
	CHECK(filter_match(&f, "kitty", "vim", 0));
	CHECK(!filter_match(&f, "foot", "vim", 0));
	CHECK(!filter_match(&f, "kitty", "bash", 0));

	f = make(NULL, NULL, "minimized");
	CHECK(filter_match(&f, "a", "b", STATUS_MINIMIZED | STATUS_ACTIVATED));
	CHECK(!filter_match(&f, "a", "b", 0));
	f = make(NULL, NULL, "!minimized");
	CHECK(!filter_match(&f, "a", "b", STATUS_MINIMIZED) && filter_match(&f, "a", "b", 0));
	f = make(NULL, NULL, "maximized,fullscreen");
	CHECK(filter_match(&f, "a", "b", STATUS_FULLSCREEN) && !filter_match(&f, "a", "b", STATUS_MINIMIZED));

	/* escaped comma and spaces around terms */
	f = make(NULL, "Inbox\\, 3 unread", NULL);
	CHECK(filter_match(&f, "a", "Inbox, 3 unread", 0) && !filter_match(&f, "a", "Inbox", 0));
	f = make("kitty, foot", NULL, NULL);
	CHECK(filter_match(&f, "foot", "", 0) && filter_match(&f, "kitty", "", 0));
	f = make(NULL, "!*secret*, *", NULL);
	CHECK(!filter_match(&f, "a", "my secret doc", 0) && filter_match(&f, "a", "doc", 0));
	f = make(NULL, NULL, "minimized, maximized");
	CHECK(filter_match(&f, "a", "b", STATUS_MAXIMIZED));

	/* unknown state never matches a status filter, other filters still work */
	CHECK(!filter_match(&f, "a", "b", STATUS_UNKNOWN));
	f = make(NULL, NULL, "!minimized");
	CHECK(!filter_match(&f, "a", "b", STATUS_UNKNOWN));
	f = make("kitty", NULL, NULL);
	CHECK(filter_match(&f, "kitty", "b", STATUS_UNKNOWN));

	/* repeated key: the last one wins */
	f = make("kitty", NULL, NULL);
	CHECK(filter_set(&f, "app-id", "foot") == 0 && filter_match(&f, "foot", "", 0) && !filter_match(&f, "kitty", "", 0));

	/* invalid input is rejected and leaves the field alone */
	f = make("kitty", NULL, NULL);
	CHECK(filter_set(&f, "status", "sticky") < 0);
	CHECK(filter_set(&f, "status", "minimized,bogus") < 0 && f.status[0] == 0);
	CHECK(filter_set(&f, "color", "red") < 0);
	CHECK(filter_set(&f, "status", "!bogus") < 0 && filter_set(&f, "status", "!") < 0);
	{
		char big[FILTER_LEN + 8];

		memset(big, 'a', sizeof(big) - 1);
		big[sizeof(big) - 1] = 0;
		CHECK(filter_set(&f, "title", big) < 0);
	}
	CHECK(strcmp(f.app_id, "kitty") == 0);

	return failures ? 1 : 0;
}
