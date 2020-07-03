/* realmd -- Realm configuration service
 *
 * Copyright 2012 Red Hat Inc
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2 of the licence or (at
 * your option) any later version.
 *
 * See the included COPYING file for more information.
 *
 * Author: Stef Walter <stefw@gnome.org>
 */

#include "config.h"

#include "service/realm-samba-config.h"
#include "service/realm-settings.h"

#include <glib/gstdio.h>

#include <string.h>

typedef struct {
	RealmIniConfig *config;
} Test;

static void
setup (Test *test,
       gconstpointer unused)
{
	test->config = realm_ini_config_new (REALM_INI_NO_WATCH | REALM_INI_LINE_CONTINUATIONS);
}

static void
teardown (Test *test,
          gconstpointer unused)
{
	g_object_unref (test->config);
}

static void
on_config_changed (RealmIniConfig *config,
                   gpointer user_data)
{
	gboolean *changed = user_data;
	*changed = TRUE;
}

static void
test_read_one (Test *test,
               gconstpointer unused)
{
	gboolean changed = FALSE;
	GError *error = NULL;
	gchar *value;
	gboolean ret;

	g_signal_connect (test->config, "changed", G_CALLBACK (on_config_changed), &changed);

	ret = realm_ini_config_read_file (test->config, TESTFILE_DIR "/smb-one.conf", &error);
	g_assert_no_error (error);
	g_assert (ret == TRUE);

	g_assert (changed == TRUE);

	value = realm_ini_config_get (test->config, "section", "one");
	g_assert_cmpstr (value, ==, "uno");
	g_free (value);
	value = realm_ini_config_get (test->config, "section", "two");
	g_assert_cmpstr (value, ==, "dos");
	g_free (value);
	value = realm_ini_config_get (test->config, "section", "three");
	g_assert_cmpstr (value, ==, "three \tThree \tTHREE");
	g_free (value);
	value = realm_ini_config_get (test->config, "section", "four");
	g_assert_cmpstr (value, ==, "cuatro");
	g_free (value);
	value = realm_ini_config_get (test->config, "section", "five");
	g_assert_cmpstr (value, ==, "cinco");
	g_free (value);
	value = realm_ini_config_get (test->config, "section", "six");
	g_assert_cmpstr (value, ==, "seis");
	g_free (value);

	/* Not present */
	value = realm_ini_config_get (test->config, "section", "zero");
	g_assert (value == NULL);

	/* Section header is broken */
	value = realm_ini_config_get (test->config, "broken", "five");
	g_assert (value == NULL);

	value = realm_ini_config_get (test->config, "another section", "ended here");
	g_assert_cmpstr (value, ==, "last");
	g_free (value);
}

static void
test_read_all (Test *test,
               gconstpointer unused)
{
	GError *error = NULL;
	GHashTable *parameters;
	gboolean ret;

	ret = realm_ini_config_read_file (test->config, TESTFILE_DIR "/smb-one.conf", &error);
	g_assert_no_error (error);
	g_assert (ret == TRUE);

	parameters = realm_ini_config_get_all (test->config, "section");
	g_assert_cmpstr (g_hash_table_lookup (parameters, "one"), ==, "uno");
	g_assert_cmpstr (g_hash_table_lookup (parameters, "two"), ==, "dos");
	g_assert_cmpstr (g_hash_table_lookup (parameters, "three"), ==, "three \tThree \tTHREE");
	g_assert_cmpstr (g_hash_table_lookup (parameters, "four"), ==, "cuatro");
	g_assert_cmpstr (g_hash_table_lookup (parameters, "five"), ==, "cinco");

	/* Section header is broken */
	parameters = realm_ini_config_get_all (test->config, "broken");
	g_assert (parameters == NULL);
}

static void
test_read_carriage_return (Test *test,
                           gconstpointer unused)
{
	const gchar *data = "[section]\n1=one\r\n2=two";
	GBytes *bytes;
	gchar *value;

	bytes = g_bytes_new_static (data, strlen (data));
	realm_ini_config_read_bytes (test->config, bytes);
	g_bytes_unref (bytes);

	value = realm_ini_config_get (test->config, "section", "1");
	g_assert_cmpstr (value, ==, "one");
	g_free (value);
	value = realm_ini_config_get (test->config, "section", "2");
	g_assert_cmpstr (value, ==, "two");
}

static void
test_read_string (Test *test,
                  gconstpointer unused)
{
	const gchar *data = "[section]\n1=one\n2=two";
	gchar *value;

	realm_ini_config_read_string (test->config, data);

	value = realm_ini_config_get (test->config, "section", "1");
	g_assert_cmpstr (value, ==, "one");
	g_free (value);
	value = realm_ini_config_get (test->config, "section", "2");
	g_assert_cmpstr (value, ==, "two");
}

static void
test_write_exact (Test *test,
                  gconstpointer unused)
{
	GError *error = NULL;
	gchar *contents;
	gsize length;
	const gchar *output;
	gsize written;
	GBytes *bytes;

	g_file_get_contents (TESTFILE_DIR "/smb-one.conf", &contents, &length, &error);
	g_assert_no_error (error);

	bytes = g_bytes_new (contents, length);
	realm_ini_config_read_bytes (test->config, bytes);
	g_bytes_unref (bytes);

	bytes = realm_ini_config_write_bytes (test->config);
	output = g_bytes_get_data (bytes, &written);
	g_assert_cmpuint (length, ==, written);
	g_assert (memcmp (contents, output, length) == 0);
	g_bytes_unref (bytes);

	g_free (contents);
}

static void
test_write_file (Test *test,
                 gconstpointer unused)
{
	GError *error = NULL;
	gchar *contents;
	gsize length;
	gchar *output;
	gsize written;
	GBytes *bytes;
	gboolean ret;

	g_file_get_contents (TESTFILE_DIR "/smb-one.conf", &contents, &length, &error);
	g_assert_no_error (error);

	bytes = g_bytes_new (contents, length);
	realm_ini_config_read_bytes (test->config, bytes);
	g_bytes_unref (bytes);

	ret = realm_ini_config_write_file (test->config, "/tmp/test-samba-config.conf", &error);
	g_assert_no_error (error);
	g_assert (ret == TRUE);

	g_file_get_contents (TESTFILE_DIR "/smb-one.conf", &output, &written, &error);
	g_assert_no_error (error);

	g_assert_cmpuint (length, ==, written);
	g_assert (memcmp (contents, output, length) == 0);

	g_free (contents);
	g_free (output);
}

static void
test_write_empty_no_create (Test *test,
                            gconstpointer unused)
{
	GError *error = NULL;
	gboolean ret;

	ret = realm_ini_config_write_file (test->config, "/non-existant", &error);
	g_assert_no_error (error);
	g_assert (ret == TRUE);

	g_assert (!g_file_test ("/non-existant", G_FILE_TEST_EXISTS));
}


static void
test_file_not_exist (Test *test,
                     gconstpointer unused)
{
	GError *error = NULL;
	gboolean ret;

	ret = realm_ini_config_read_file (test->config, "/non-existant", &error);
	g_assert_no_error (error);
	g_assert (ret == TRUE);
}

static gboolean
on_timeout_quit_loop (gpointer user_data)
{
	g_main_loop_quit (user_data);
	return FALSE; /* don't call again */
}

static void
test_file_watch (Test *test,
                 gconstpointer unused)
{
	const gchar *data = "[section]\nkey=12345";
	const gchar *filename = "/tmp/test-samba-config.watch";
	gboolean changed = FALSE;
	GError *error = NULL;
	GMainLoop *loop;
	gchar *value;
	gboolean ret;

	g_unlink (filename);
	ret = realm_ini_config_read_file (test->config, filename, &error);
	g_assert_no_error (error);
	g_assert (ret == TRUE);

	g_signal_connect (test->config, "changed", G_CALLBACK (on_config_changed), &changed);

	value = realm_ini_config_get (test->config, "section", "key");
	g_assert (value == NULL);

	/* Now write to the file */
	g_file_set_contents (filename, data, -1, &error);
	g_assert_no_error (error);

	/* Wait a couple seconds */
	loop = g_main_loop_new (NULL, FALSE);
	g_timeout_add_seconds (2, on_timeout_quit_loop, loop);
	g_main_loop_run (loop);
	g_main_loop_unref (loop);

	g_assert (changed == TRUE);
	value = realm_ini_config_get (test->config, "section", "key");
	g_assert_cmpstr (value, ==, "12345");
	g_free (value);
}

static void
test_set (Test *test,
          gconstpointer unused)
{
	const gchar *data = "[section]\n\t1= one\r\n2=two\n3=three";
	const gchar *check = "[section]\n1 = the number one\n2=two\n4 = four\n";
	gboolean changed = FALSE;
	gchar *output;

	realm_ini_config_read_string (test->config, data);

	g_signal_connect (test->config, "changed", G_CALLBACK (on_config_changed), &changed);

	realm_ini_config_set (test->config, "section", "1", "the number one", NULL);
	realm_ini_config_set (test->config, "section", "3", NULL,
	                      "4", "four", NULL);

	g_assert (changed == TRUE);

	output = realm_ini_config_write_string (test->config);
	g_assert_cmpstr (check, ==, output);
	g_free (output);
}

static void
test_set_middle (Test *test,
                 gconstpointer unused)
{
	const gchar *data = "[section]\n1=one\n2=two\n\n[another]\n4=four";
	const gchar *check = "[section]\n1=one\n2=two\n3 = three\n\n[another]\n4=four";
	gchar *output;

	realm_ini_config_read_string (test->config, data);

	realm_ini_config_set (test->config, "section", "3", "three", NULL);

	output = realm_ini_config_write_string (test->config);
	g_assert_cmpstr (check, ==, output);
	g_free (output);
}

static void
test_set_and_get (Test *test,
                  gconstpointer unused)
{
	const gchar *data = "[section]\n1=one\n2=two\n\n[another]\n4=four";
	gchar *output;

	realm_ini_config_read_string (test->config, data);

	realm_ini_config_set (test->config, "section", "3", "three", NULL);

	output = realm_ini_config_get (test->config, "section", "3");
	g_assert_cmpstr (output, ==, "three");
	g_free (output);
}

static void
test_have (Test *test,
          gconstpointer unused)
{
	const gchar *data = "[section]\n\t1= one\r\n2=two\n3=three";
	realm_ini_config_read_string (test->config, data);

	g_assert_cmpint (realm_ini_config_have (test->config, "section", "1"), ==, TRUE);
	g_assert_cmpint (realm_ini_config_have (test->config, "section", "not there"), ==, FALSE);
	g_assert_cmpint (realm_ini_config_have (test->config, "invalid", "2"), ==, FALSE);
}

static void
test_set_section (Test *test,
                  gconstpointer unused)
{
	const gchar *data = "[section]\n1=one\n2=two";
	const gchar *check = "[section]\n1=one\n2=two\n\n[happy]\n4 = four\n";
	gchar *output;

	realm_ini_config_read_string (test->config, data);

	realm_ini_config_set (test->config, "happy", "4", "four", NULL);
	realm_ini_config_set (test->config, "nope", "6", NULL, NULL);

	output = realm_ini_config_write_string (test->config);
	g_assert_cmpstr (check, ==, output);
	g_free (output);
}

static void
test_set_all (Test *test,
              gconstpointer unused)
{
	const gchar *data = "[section]\n\t1= one\r\n2=two\n3=three";
	const gchar *check = "[section]\n1 = the number one\n2=two\n4 = four\n";
	gboolean changed = FALSE;
	gchar *output;
	GHashTable *parameters;

	realm_ini_config_read_string (test->config, data);

	g_signal_connect (test->config, "changed", G_CALLBACK (on_config_changed), &changed);

	parameters = g_hash_table_new (g_str_hash, g_str_equal);
	g_hash_table_insert (parameters, "1", "the number one");
	g_hash_table_insert (parameters, "3", NULL);
	g_hash_table_insert (parameters, "4", "four");
	realm_ini_config_set_all (test->config, "section", parameters);
	g_hash_table_unref (parameters);

	g_assert (changed == TRUE);

	output = realm_ini_config_write_string (test->config);
	g_assert_cmpstr (check, ==, output);
	g_free (output);
}

static void
test_get_sections (Test *test,
                   gconstpointer unused)
{
	const gchar *data = "[section]\n\t1= one\r\n2=two\n3=three\n[section2]\n[three]";
	gchar **sections;

	realm_ini_config_read_string (test->config, data);
	sections = realm_ini_config_get_sections (test->config);
	g_assert (sections != NULL);
	g_assert_cmpint (g_strv_length (sections), ==, 3);
	g_strfreev (sections);
}

static void
test_have_section (Test *test,
                   gconstpointer unused)
{
	const gchar *data = "[section]\n\t1= one\r\n2=two\n3=three";

	realm_ini_config_read_string (test->config, data);
	g_assert (realm_ini_config_have_section (test->config, "section") == TRUE);
	g_assert (realm_ini_config_have_section (test->config, "nonexistant") == FALSE);
}

static void
test_remove_section_first (Test *test,
                            gconstpointer unused)
{
	const gchar *data = "[first]\nblah=blue\n#A\n[section]\n\t1=I\n#B\n2=II\n#C\n[after]\n2=two";
	const gchar *check = "#A\n[section]\n\t1=I\n#B\n2=II\n#C\n[after]\n2=two";
	gchar *output;

	realm_ini_config_read_string (test->config, data);
	realm_ini_config_remove_section (test->config, "first");

	output = realm_ini_config_write_string (test->config);
	g_assert_cmpstr (check, ==, output);
	g_free (output);
}

static void
test_remove_section_middle (Test *test,
                            gconstpointer unused)
{
	const gchar *data = "[first]\nblah=blue\n#A\n[section]\n\t1=I\n#B\n2=II\n#C\n[after]\n2=two";
	const gchar *check = "[first]\nblah=blue\n#A\n#C\n[after]\n2=two";
	gchar *output;

	realm_ini_config_read_string (test->config, data);
	realm_ini_config_remove_section (test->config, "section");

	output = realm_ini_config_write_string (test->config);
	g_assert_cmpstr (check, ==, output);
	g_free (output);
}

static void
test_remove_section_last (Test *test,
                          gconstpointer unused)
{
	const gchar *data = "[first]\nblah=blue\n#A\n[section]\n\t1=I\n#B\n2=II\n#C\n[after]\n2=two";
	const gchar *check = "[first]\nblah=blue\n#A\n[section]\n\t1=I\n#B\n2=II\n#C\n";
	gchar *output;

	realm_ini_config_read_string (test->config, data);
	realm_ini_config_remove_section (test->config, "after");

	output = realm_ini_config_write_string (test->config);
	g_assert_cmpstr (check, ==, output);
	g_free (output);
}

static void
test_remove_section_all (Test *test,
                         gconstpointer unused)
{
	const gchar *data = "#A\n[section]\n\t1=I\n#B\n2=II\n#C\n";
	const gchar *check = "#A\n#C\n";
	gchar *output;

	realm_ini_config_read_string (test->config, data);
	realm_ini_config_remove_section (test->config, "section");

	output = realm_ini_config_write_string (test->config);
	g_assert_cmpstr (check, ==, output);
	g_free (output);
}

static void
test_remove_section_not_exist (Test *test,
                               gconstpointer unused)
{
	const gchar *data = "[first]\nblah=blue\n# Comment\n[section]\n\t1=I\n#C\n2=II\n# Comment\n[after]\n2=two";
	gchar *output;

	realm_ini_config_read_string (test->config, data);
	realm_ini_config_remove_section (test->config, "nonexistant");

	output = realm_ini_config_write_string (test->config);
	g_assert_cmpstr (data, ==, output);
	g_free (output);
}

static void
test_change (Test *test,
             gconstpointer unused)
{
	const gchar *data = "[section]\n\t1= one\r\n2=two\n3=three";
	const gchar *check = "[section]\n1 = the number one\n2=two\n4 = four\n";
	GError *error = NULL;
	gchar *output;

	/* Setup this file as the system smb.conf */
	g_file_set_contents ("/tmp/test-samba-config.conf", data, -1, &error);
	g_assert_no_error (error);

	realm_ini_config_set_filename (test->config, "/tmp/test-samba-config.conf");
	realm_ini_config_change (test->config, "section", &error,
	                         "1", "the number one",
	                         "3", NULL,
	                         "4", "four",
	                         NULL);
	g_assert_no_error (error);

	g_file_get_contents ("/tmp/test-samba-config.conf", &output, NULL, &error);
	g_assert_no_error (error);

	g_assert_cmpstr (output, ==, check);
	g_free (output);
}

static void
test_change_list (Test *test,
                  gconstpointer unused)
{
	const gchar *data = "[section]\n\t1= one\n2=two, dos,zwei ,duo\n3=three";
	const gchar *check = "[section]\n\t1= one\n2 = dos, zwei, 10\n3=three";
	const gchar *remove[] = { "two", "duo", NULL };
	const gchar *add[] = { "TWO", "10", NULL };
	GError *error = NULL;
	gchar *output;

	/* Setup this file as the system smb.conf */
	g_file_set_contents ("/tmp/test-samba-config.conf", data, -1, &error);
	g_assert_no_error (error);

	realm_ini_config_set_filename (test->config, "/tmp/test-samba-config.conf");
	realm_ini_config_change_list (test->config, "section", "2", ",",
	                              add, remove, &error);
	g_assert_no_error (error);

	g_file_get_contents ("/tmp/test-samba-config.conf", &output, NULL, &error);
	g_assert_no_error (error);

	g_assert_cmpstr (output, ==, check);
	g_free (output);
}

static void
test_change_list_new (Test *test,
                      gconstpointer unused)
{
	const gchar *data = "[section]\n\t1= one\n3=three";
	const gchar *check = "[section]\n\t1= one\n3=three\n2 = dos, zwei, 10\n";
	const gchar **remove = NULL;
	const gchar *add[] = { "dos", "zwei", "10", NULL };
	GError *error = NULL;
	gchar *output;

	/* Setup this file as the system smb.conf */
	realm_settings_add ("paths", "smb.conf", "/tmp/test-samba-config.conf");
	g_file_set_contents ("/tmp/test-samba-config.conf", data, -1, &error);
	g_assert_no_error (error);

	realm_ini_config_set_filename (test->config, "/tmp/test-samba-config.conf");
	realm_ini_config_change_list (test->config, "section", "2", ",",
	                              add, remove, &error);
	g_assert_no_error (error);

	g_file_get_contents ("/tmp/test-samba-config.conf", &output, NULL, &error);
	g_assert_no_error (error);

	g_assert_cmpstr (output, ==, check);
	g_free (output);
}

static void
test_change_list_null_add (Test *test,
                           gconstpointer unused)
{
	const gchar *data = "[section]\n\t1= one\n2=two, dos,zwei ,duo\n3=three";
	const gchar *check = "[section]\n\t1= one\n2 = dos, zwei\n3=three";
	const gchar *remove[] = { "two", "duo", NULL };
	const gchar **add = NULL;
	GError *error = NULL;
	gchar *output;

	/* Setup this file as the system smb.conf */
	realm_settings_add ("paths", "smb.conf", "/tmp/test-samba-config.conf");
	g_file_set_contents ("/tmp/test-samba-config.conf", data, -1, &error);
	g_assert_no_error (error);

	realm_ini_config_set_filename (test->config, "/tmp/test-samba-config.conf");
	realm_ini_config_change_list (test->config, "section", "2", ",",
	                              add, remove, &error);
	g_assert_no_error (error);

	g_file_get_contents ("/tmp/test-samba-config.conf", &output, NULL, &error);
	g_assert_no_error (error);

	g_assert_cmpstr (output, ==, check);
	g_free (output);
}

static void
test_change_list_null_remove (Test *test,
                              gconstpointer unused)
{
	const gchar *data = "[section]\n\t1= one\n2=two, dos,zwei ,duo\n3=three";
	const gchar *check = "[section]\n\t1= one\n2 = two, dos, zwei, duo, 10\n3=three";
	const gchar **remove = NULL;
	const gchar *add[] = { "TWO", "10", NULL };
	GError *error = NULL;
	gchar *output;

	/* Setup this file as the system smb.conf */
	realm_settings_add ("paths", "smb.conf", "/tmp/test-samba-config.conf");
	g_file_set_contents ("/tmp/test-samba-config.conf", data, -1, &error);
	g_assert_no_error (error);

	realm_ini_config_set_filename (test->config, "/tmp/test-samba-config.conf");
	realm_ini_config_change_list (test->config, "section", "2", ",",
	                              add, remove, &error);
	g_assert_no_error (error);

	g_file_get_contents ("/tmp/test-samba-config.conf", &output, NULL, &error);
	g_assert_no_error (error);

	g_assert_cmpstr (output, ==, check);
	g_free (output);
}

static void
test_get_boolean (void)
{
	RealmIniConfig *config;

	config = realm_ini_config_new (0);

	realm_ini_config_read_string (config, "[section]\nboolean = true");
	g_assert_cmpint (TRUE, ==, realm_ini_config_get_boolean (config, "section", "boolean", FALSE));

	realm_ini_config_read_string (config, "[section]\nboolean = FalSE");
	g_assert_cmpint (FALSE, ==, realm_ini_config_get_boolean (config, "section", "boolean", TRUE));

	realm_ini_config_read_string (config, "[section]\nboolean = false");
	g_assert_cmpint (FALSE, ==, realm_ini_config_get_boolean (config, "section", "boolean", TRUE));

	realm_ini_config_read_string (config, "[section]\nboolean = false");
	g_assert_cmpint (TRUE, ==, realm_ini_config_get_boolean (config, "section", "non-existant", TRUE));

	realm_ini_config_read_string (config, "[section]\nboolean = false");
	g_assert_cmpint (TRUE, ==, realm_ini_config_get_boolean (config, "section", "non-existant", TRUE));

	realm_ini_config_read_string (config, "[section]\nboolean = yes");
	g_assert_cmpint (TRUE, ==, realm_ini_config_get_boolean (config, "section", "boolean", TRUE));

	realm_ini_config_read_string (config, "[section]\nboolean = no");
	g_assert_cmpint (FALSE, ==, realm_ini_config_get_boolean (config, "section", "boolean", FALSE));

	realm_ini_config_read_string (config, "[section]\nboolean = 1");
	g_assert_cmpint (TRUE, ==, realm_ini_config_get_boolean (config, "section", "boolean", TRUE));

	realm_ini_config_read_string (config, "[section]\nboolean = 0");
	g_assert_cmpint (FALSE, ==, realm_ini_config_get_boolean (config, "section", "boolean", FALSE));

	g_object_unref (config);
}

int
main (int argc,
      char **argv)
{
#if !GLIB_CHECK_VERSION(2, 36, 0)
	g_type_init ();
#endif
	g_test_init (&argc, &argv, NULL);
	g_set_prgname ("test-ini-config");

	realm_settings_init ();

	g_test_add ("/realmd/ini-config/read-one", Test, NULL, setup, test_read_one, teardown);
	g_test_add ("/realmd/ini-config/read-all", Test, NULL, setup, test_read_all, teardown);
	g_test_add ("/realmd/ini-config/read-string", Test, NULL, setup, test_read_string, teardown);
	g_test_add ("/realmd/ini-config/read-carriage-return", Test, NULL, setup, test_read_carriage_return, teardown);

	g_test_add ("/realmd/ini-config/write-exact", Test, NULL, setup, test_write_exact, teardown);
	g_test_add ("/realmd/ini-config/write-file", Test, NULL, setup, test_write_file, teardown);
	g_test_add ("/realmd/ini-config/write-empty-no-create", Test, NULL, setup, test_write_empty_no_create, teardown);

	g_test_add ("/realmd/ini-config/have", Test, NULL, setup, test_have, teardown);

	g_test_add ("/realmd/ini-config/set", Test, NULL, setup, test_set, teardown);
	g_test_add ("/realmd/ini-config/set-middle", Test, NULL, setup, test_set_middle, teardown);
	g_test_add ("/realmd/ini-config/set-and-get", Test, NULL, setup, test_set_and_get, teardown);
	g_test_add ("/realmd/ini-config/set-section", Test, NULL, setup, test_set_section, teardown);
	g_test_add ("/realmd/ini-config/set-all", Test, NULL, setup, test_set_all, teardown);

	g_test_add ("/realmd/ini-config/get-sections", Test, NULL, setup, test_get_sections, teardown);
	g_test_add ("/realmd/ini-config/have-section", Test, NULL, setup, test_have_section, teardown);
	g_test_add ("/realmd/ini-config/remove-section-first", Test, NULL, setup, test_remove_section_first, teardown);
	g_test_add ("/realmd/ini-config/remove-section-middle", Test, NULL, setup, test_remove_section_middle, teardown);
	g_test_add ("/realmd/ini-config/remove-section-last", Test, NULL, setup, test_remove_section_last, teardown);
	g_test_add ("/realmd/ini-config/remove-section-all", Test, NULL, setup, test_remove_section_all, teardown);
	g_test_add ("/realmd/ini-config/remove-section-not-exist", Test, NULL, setup, test_remove_section_not_exist, teardown);

	g_test_add ("/realmd/ini-config/file-not-exist", Test, NULL, setup, test_file_not_exist, teardown);
	if (!g_test_quick ())
		g_test_add ("/realmd/ini-config/file-watch", Test, NULL, setup, test_file_watch, teardown);

	g_test_add ("/realmd/ini-config/change", Test, NULL, setup, test_change, teardown);
	g_test_add ("/realmd/ini-config/change-list", Test, NULL, setup, test_change_list, teardown);
	g_test_add ("/realmd/ini-config/change-list-new", Test, NULL, setup, test_change_list_new, teardown);
	g_test_add ("/realmd/ini-config/change-list-null-add", Test, NULL, setup, test_change_list_null_add, teardown);
	g_test_add ("/realmd/ini-config/change-list-null-remove", Test, NULL, setup, test_change_list_null_remove, teardown);

	g_test_add_func ("/realmd/ini-config/get-boolean", test_get_boolean);

	return g_test_run ();
}
