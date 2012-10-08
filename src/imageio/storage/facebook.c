/*
    This file is part of darktable,
    copyright (c) 2012 Pierre Lamot

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "dtgtk/button.h"
#include "dtgtk/label.h"
#include "gui/gtk.h"
#include "common/darktable.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/imageio_module.h"
#include "common/imageio.h"
#include "common/tags.h"
#include "common/pwstorage/pwstorage.h"
#include "common/metadata.h"
#include "control/conf.h"
#include "control/control.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <curl/curl.h>
#include <json-glib/json-glib.h>

DT_MODULE(1)

#define FB_WS_BASE_URL "https://www.facebook.com/"
#define FB_GRAPH_BASE_URL "https://graph.facebook.com/"
#define FB_API_KEY "315766121847254"

#define FB_IMAGE_MAX_SIZE 960

#define MSGCOLOR_RED "#e07f7f"
#define MSGCOLOR_GREEN "#7fe07f"

typedef enum ComboUserModel
{
  COMBO_USER_MODEL_NAME_COL = 0,
  COMBO_USER_MODEL_TOKEN_COL,
  COMBO_USER_MODEL_ID_COL,
  COMBO_USER_MODEL_NB_COL
} ComboUserModel;

typedef enum ComboAlbumModel
{
  COMBO_ALBUM_MODEL_NAME_COL = 0,
  COMBO_ALBUM_MODEL_ID_COL,
  COMBO_ALBUM_MODEL_NB_COL
} ComboAlbumModel;

typedef enum ComboPrivacyModel
{
  COMBO_PRIVACY_MODEL_NAME_COL = 0,
  COMBO_PRIVACY_MODEL_VAL_COL,
  COMBO_PRIVACY_MODEL_NB_COL
} ComboPrivacyModel;


/*
 * note: we don't support some kinds of privacy setting:
 *  CUSTOM : no plan to do this one for the moment
 *  NETWORKS_FRIENDS : this seems to be deprecated, it is currently impossible
 *      to create a new network (https://www.facebook.com/help/networks)
 */
typedef enum FBAlbumPrivacyPolicy
{
  FBALBUM_PRIVACY_EVERYONE,
  FBALBUM_PRIVACY_ALL_FRIENDS,
  FBALBUM_PRIVACY_NETWORKS_FRIENDS, //not implemented
  FBALBUM_PRIVACY_FRIENDS_OF_FRIENDS,
  FBALBUM_PRIVACY_SELF,
  FBALBUM_PRIVACY_CUSTOM //not implemented
} FBAlbumPrivacyPolicy;


/**
 * Represents informations about an album
 */
typedef struct FBAlbum {
  gchar *id;
  gchar *name;
  FBAlbumPrivacyPolicy privacy;
} FBAlbum;

FBAlbum *fb_album_init()
{
  return  (FBAlbum*) g_malloc0(sizeof(FBAlbum));
}

void fb_album_destroy(FBAlbum *album)
{
  if (album == NULL)
    return;
  g_free(album->id);
  g_free(album->name);
  g_free(album);
}

/**
 * Represents informations about an account
 */
typedef struct FBAccountInfo {
  gchar *id;
  gchar *username;
  gchar *token;
} FBAccountInfo;

FBAccountInfo *fb_account_info_init()
{
  return (FBAccountInfo*) g_malloc0(sizeof(FBAccountInfo));
}

void fb_account_info_destroy(FBAccountInfo *account)
{
  if (account == NULL)
    return;
  g_free(account->id);
  g_free(account->username);
  g_free(account);
}

typedef struct FBContext
{
  /// curl context
  CURL *curl_ctx;
  /// Json parser context
  JsonParser *json_parser;

  GString *errmsg;

  /// authorization token
  gchar *token;

  gchar *album_id;
  char *album_title;
  char *album_summary;
  int album_permission;
  gboolean new_album;
} FBContext;

typedef struct dt_storage_facebook_gui_data_t
{
  // == ui elements ==
  GtkLabel *label_username;
  GtkLabel *label_album;
  GtkLabel *label_status;

  GtkComboBox *comboBox_username;
  GtkButton *button_login;

  GtkDarktableButton *dtbutton_refresh_album;
  GtkComboBox *comboBox_album;

  //  === album creation section ===
  GtkLabel *label_album_title;
  GtkLabel *label_album_summary;
  GtkLabel *label_album_privacy;

  GtkEntry *entry_album_title;
  GtkEntry *entry_album_summary;
  GtkComboBox *comboBox_privacy;

  GtkBox *hbox_album;

  // == context ==
  gboolean connected;
  FBContext *facebook_api;
} dt_storage_facebook_gui_data_t;


static FBContext *fb_api_init()
{
  FBContext *ctx = (FBContext*)g_malloc0(sizeof(FBContext));
  ctx->curl_ctx = curl_easy_init();
  ctx->errmsg = g_string_new("");
  ctx->json_parser = json_parser_new();
  return ctx;
}


static void fb_api_destroy(FBContext *ctx)
{
  if (ctx == NULL)
    return;
  curl_easy_cleanup(ctx->curl_ctx);
  g_free(ctx->token);
  g_object_unref(ctx->json_parser);
  g_string_free(ctx->errmsg, TRUE);
  g_free(ctx);
}


typedef struct dt_storage_facebook_param_t
{
  gint64 hash;
  FBContext *facebook_ctx;
} dt_storage_facebook_param_t;


//////////////////////////// curl requests related functions

/**
 * extract the user token from the calback @a url
 */
static gchar *fb_extract_token_from_url(const gchar *url)
{
  g_return_val_if_fail((url != NULL), NULL);
  if(!(g_str_has_prefix(url, FB_WS_BASE_URL "connect/login_success.html") == TRUE)) return NULL;

  char *authtoken = NULL;

  char* *urlchunks = g_strsplit_set(url, "#&=", -1);
  //starts at 1 to skip the url prefix, then values are in the form key=value
  for (int i = 1; urlchunks[i] != NULL; i += 2)
  {
    if ((g_strcmp0(urlchunks[i], "access_token") == 0) && (urlchunks[i + 1] != NULL))
    {
      authtoken = g_strdup(urlchunks[i + 1]);
      break;
    }
    else if (g_strcmp0(urlchunks[i], "error") == 0)
    {
      break;
    }
    if (urlchunks[i + 1] == NULL) //this shouldn't happens but we never know...
    {
      g_printerr(_("[facebook] unexpeted url format\n"));
      break;
    }
  }
  g_strfreev(urlchunks);
  return authtoken;
}

static size_t curl_write_data_cb(void *ptr, size_t size, size_t nmemb, void *data)
{
  GString *string = (GString*) data;
  g_string_append_len(string, ptr, size * nmemb);
#ifdef FACEBOOK_EXTRA_VERBOSE
  g_printf("server reply: %s\n", string->str);
#endif
  return size * nmemb;
}

static JsonObject *fb_parse_response(FBContext *ctx, GString *response)
{
  GError *error;
  gboolean ret = json_parser_load_from_data(ctx->json_parser, response->str, response->len, &error);
  g_return_val_if_fail((ret), NULL);

  JsonNode *root = json_parser_get_root(ctx->json_parser);
  //we should always have a dict
  g_return_val_if_fail((json_node_get_node_type(root) == JSON_NODE_OBJECT), NULL);

  JsonObject *rootdict = json_node_get_object(root);
  if (json_object_has_member(rootdict, "error"))
  {
    JsonObject *errorstruct = json_object_get_object_member(rootdict, "error");
    g_return_val_if_fail((errorstruct != NULL), NULL);
    const gchar *errormessage = json_object_get_string_member(errorstruct, "message");
    g_return_val_if_fail((errormessage != NULL), NULL);
    g_string_assign(ctx->errmsg, errormessage);
    return NULL;
  }

  return rootdict;
}


static void fb_query_get_add_url_arguments(GString *key, GString *value, GString *url)
{
  g_string_append(url, "&");
  g_string_append(url, key->str);
  g_string_append(url, "=");
  g_string_append(url, value->str);
}

/**
 * perform a GET request on facebook graph api
 *
 * @note use this one to read information (user info, existing albums, ...)
 *
 * @param ctx facebook context (token field must be set)
 * @param method the method to call on the facebook graph API, the methods should not start with '/' example: "me/albums"
 * @param args hashtable of the aguments to be added to the requests, must be in the form key (string) = value (string)
 * @returns NULL if the request fails, or a JsonObject of the reply
 */
static JsonObject *fb_query_get(FBContext *ctx, const gchar *method, GHashTable *args)
{
  g_return_val_if_fail(ctx != NULL, NULL);
  g_return_val_if_fail(ctx->token != NULL, NULL);
  //build the query
  GString *url = g_string_new(FB_GRAPH_BASE_URL);
  g_string_append(url, method);
  g_string_append(url, "?access_token=");
  g_string_append(url, ctx->token);
  if (args != NULL)
    g_hash_table_foreach(args, (GHFunc)fb_query_get_add_url_arguments, url);

  //send the request
  GString *response = g_string_new("");
  curl_easy_reset(ctx->curl_ctx);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_URL, url->str);
#ifdef FACEBOOK_EXTRA_VERBOSE
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_VERBOSE, 2);
#endif
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_WRITEFUNCTION, curl_write_data_cb);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_SSL_VERIFYPEER, FALSE);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_WRITEDATA, response);
  int res = curl_easy_perform(ctx->curl_ctx);

  if (res != CURLE_OK) return NULL;

  //parse the response
  JsonObject *respobj = fb_parse_response(ctx, response);

  g_string_free(response, TRUE);
  g_string_free(url, TRUE);
  return respobj;
}

typedef struct {
  struct curl_httppost *formpost;
  struct curl_httppost *lastptr;
} HttppostFormList;

static void fb_query_post_add_form_arguments(const gchar *key, const gchar *value, HttppostFormList *formlist)
{
  curl_formadd(&(formlist->formpost),
    &(formlist->lastptr),
    CURLFORM_COPYNAME, key,
    CURLFORM_COPYCONTENTS, value,
    CURLFORM_END);
}

static void fb_query_post_add_file_arguments(const gchar *key, const gchar *value, HttppostFormList *formlist)
{
  curl_formadd(&(formlist->formpost),
    &(formlist->lastptr),
    CURLFORM_COPYNAME, key,
    CURLFORM_COPYCONTENTS, value,
    CURLFORM_END);

  curl_formadd(&(formlist->formpost),
    &(formlist->lastptr),
    CURLFORM_COPYNAME, key,
    CURLFORM_FILE, value,
    CURLFORM_END);
}

/**
 * perform a POST request on facebook graph api
 *
 * @note use this one to create object (album, photos, ...)
 *
 * @param ctx facebook context (token field must be set)
 * @param method the method to call on the facebook graph API, the methods should not start with '/' example: "me/albums"
 * @param args hashtable of the aguments to be added to the requests, might be null if none
 * @param files hashtable of the files to be send with the requests, might be null if none
 * @returns NULL if the request fails, or a JsonObject of the reply
 */
static JsonObject *fb_query_post(FBContext *ctx, const gchar *method, GHashTable *args, GHashTable *files)
{
  g_return_val_if_fail(ctx != NULL, NULL);
  g_return_val_if_fail(ctx->token != NULL, NULL);

  GString *url =g_string_new(FB_GRAPH_BASE_URL);
  g_string_append(url, method);

  HttppostFormList formlist;
  formlist.formpost = NULL;
  formlist.lastptr = NULL;

  curl_formadd(&(formlist.formpost),
    &(formlist.lastptr),
    CURLFORM_COPYNAME, "access_token",
    CURLFORM_COPYCONTENTS, ctx->token,
    CURLFORM_END);
  if (args != NULL)
    g_hash_table_foreach(args, (GHFunc)fb_query_post_add_form_arguments, &formlist);

  if (files != NULL)
    g_hash_table_foreach(files, (GHFunc)fb_query_post_add_file_arguments, &formlist);

  //send the requests
  GString *response = g_string_new("");
  curl_easy_reset(ctx->curl_ctx);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_URL, url->str);
#ifdef FACEBOOK_EXTRA_VERBOSE
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_VERBOSE, 2);
#endif
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_HTTPPOST, formlist.formpost);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_SSL_VERIFYPEER, FALSE);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_WRITEFUNCTION, curl_write_data_cb);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_WRITEDATA, response);
  int res = curl_easy_perform(ctx->curl_ctx);
  curl_formfree(formlist.formpost);
  g_string_free(url, TRUE);
  if (res != CURLE_OK) return NULL;
  //parse the response
  JsonObject *respobj = fb_parse_response(ctx, response);
  g_string_free(response, TRUE);
  return respobj;
}

//////////////////////////// facebook api functions

/**
 * @returns TRUE if the current token is valid
 */
static gboolean fb_test_auth_token(FBContext *ctx)
{
  JsonObject *obj = fb_query_get(ctx, "me", NULL);
  return obj != NULL;
}

/**
 * @return a GList of FBAlbums associated to the user
 */
static GList *fb_get_album_list(FBContext *ctx, gboolean* ok)
{
  if (ok) *ok = TRUE;
  GList *album_list = NULL;

  JsonObject *reply = fb_query_get(ctx, "me/albums", NULL);
  if (reply == NULL)
    goto error;

  JsonArray *jsalbums = json_object_get_array_member(reply, "data");
  if (jsalbums == NULL)
    goto error;

  guint i;
  for (i = 0; i < json_array_get_length(jsalbums); i++)
  {
    JsonObject *obj = json_array_get_object_element(jsalbums, i);
    if (obj == NULL)
      continue;

    JsonNode* canupload_node = json_object_get_member(obj, "can_upload");
    if (canupload_node == NULL || !json_node_get_boolean(canupload_node))
      continue;

    FBAlbum *album = fb_album_init();
    const char* id = json_object_get_string_member(obj, "id");
    const char* name = json_object_get_string_member(obj, "name");
    if (id == NULL || name == NULL)
      goto error;
    album->id = g_strdup(id);
    album->name = g_strdup(name);
    album_list = g_list_append(album_list, album);
  }
  return album_list;

error:
  *ok = FALSE;
  g_list_free_full(album_list, (GDestroyNotify)fb_album_destroy);
  return NULL;
}

/**
 * @see https://developers.facebook.com/docs/reference/api/user/
 * @return the id of the newly reacted
 */
static const gchar *fb_create_album(FBContext *ctx, gchar *name, gchar *summary, FBAlbumPrivacyPolicy privacy)
{
  GHashTable *args = g_hash_table_new((GHashFunc)g_str_hash, (GEqualFunc)g_str_equal);
  g_hash_table_insert(args, "name", name);
  if (summary != NULL)
    g_hash_table_insert(args, "message", summary);
  switch (privacy)
  {
    case FBALBUM_PRIVACY_EVERYONE:
      g_hash_table_insert(args, "privacy", "{\"value\":\"EVERYONE\"}");
      break;
    case FBALBUM_PRIVACY_ALL_FRIENDS:
      g_hash_table_insert(args, "privacy", "{\"value\":\"ALL_FRIENDS\"}");
      break;
    case FBALBUM_PRIVACY_FRIENDS_OF_FRIENDS:
      g_hash_table_insert(args, "privacy", "{\"value\":\"FRIENDS_OF_FRIENDS\"}");
      break;
    case FBALBUM_PRIVACY_SELF:
      g_hash_table_insert(args, "privacy", "{\"value\":\"SELF\"}");
      break;
    default:
      goto error;
      break;
  }
  JsonObject *ref = fb_query_post(ctx, "me/albums", args, NULL);
  if (ref == NULL)
    goto error;
  g_hash_table_destroy(args);
  return json_object_get_string_member(ref, "id");

error:
  g_hash_table_destroy(args);
  return NULL;
}

/**
 * @see https://developers.facebook.com/docs/reference/api/album/
 * @return the id of the uploaded photo
 */
static const gchar *fb_upload_photo_to_album(FBContext *ctx, gchar *albumid, gchar *fpath, gchar *description)
{
  GString *method = g_string_new(albumid);
  g_string_append(method, "/photos");

  GHashTable *files = g_hash_table_new((GHashFunc)g_str_hash, (GEqualFunc)g_str_equal);
  g_hash_table_insert(files, "source", fpath);

  GHashTable *args = NULL;
  if (description != NULL)
  {
    args = g_hash_table_new((GHashFunc)g_str_hash, (GEqualFunc)g_str_equal);
    g_hash_table_insert(args, "message", description);
  }

  JsonObject *ref = fb_query_post(ctx, method->str, args, files);
  g_string_free(method, TRUE);

  g_hash_table_destroy(files);
  if (args != NULL)
  {
    g_hash_table_destroy(args);
  }
  if (ref == NULL)
    return NULL;
  return json_object_get_string_member(ref, "id");
}

/**
 * @see https://developers.facebook.com/docs/reference/api/user/
 * @return basic informations about the account
 */
static FBAccountInfo *fb_get_account_info(FBContext *ctx)
{
  JsonObject *obj = fb_query_get(ctx, "me", NULL);
  g_return_val_if_fail((obj != NULL), NULL);
  const gchar *user_name = json_object_get_string_member(obj, "username");
  const gchar *user_id = json_object_get_string_member(obj, "id");
  g_return_val_if_fail(user_name != NULL && user_id != NULL, NULL);
  FBAccountInfo *accountinfo = fb_account_info_init();
  accountinfo->id = g_strdup(user_id);
  accountinfo->username = g_strdup(user_name);
  accountinfo->token = g_strdup(ctx->token);
  return accountinfo;
}


///////////////////////////////// UI functions

static gboolean combobox_separator(GtkTreeModel *model,GtkTreeIter *iter,gpointer data)
{
  GValue value = { 0, };
  gtk_tree_model_get_value(model,iter,0,&value);
  gchar *v=NULL;
  if (G_VALUE_HOLDS_STRING (&value))
  {
    if( (v=(gchar *)g_value_get_string (&value))!=NULL && strlen(v) == 0 ) return TRUE;
  }
  return FALSE;
}

/**
 * @see https://developers.facebook.com/docs/authentication/
 * @returs NULL if the user cancel the operation or a valid token
 */
static gchar *facebook_get_user_auth_token(dt_storage_facebook_gui_data_t *ui)
{
  ///////////// open the authentication url in a browser
  GError *error = NULL;
  gtk_show_uri(gdk_screen_get_default(),
               FB_WS_BASE_URL"dialog/oauth?"
               "client_id=" FB_API_KEY
               "&redirect_uri="FB_WS_BASE_URL"connect/login_success.html"
               "&scope=user_photos,publish_stream"
               "&response_type=token", gtk_get_current_event_time(), &error);

  ////////////// build & show the validation dialog
  gchar *text1 = _("step 1: a new window or tab of your browser should have been"
                   "loaded. you have to login into your facebook account there "
                   "and authorize darktable to upload photos before continuing.");
  gchar *text2 = _("step 2: paste your browser url and click the ok button once "
                   "you are done.");

  GtkWidget *window = dt_ui_main_window(darktable.gui->ui);
  GtkDialog *fb_auth_dialog = GTK_DIALOG(gtk_message_dialog_new (GTK_WINDOW (window),
                                  GTK_DIALOG_DESTROY_WITH_PARENT,
                                  GTK_MESSAGE_QUESTION,
                                  GTK_BUTTONS_OK_CANCEL,
                                  _("Facebook authentication")));
  gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (fb_auth_dialog),
      "%s\n\n%s", text1, text2);

  GtkWidget *entry = gtk_entry_new();
  GtkWidget *hbox = gtk_hbox_new(FALSE, 5);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(gtk_label_new(_("url:"))), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(entry), TRUE, TRUE, 0);
  gtk_box_pack_end(GTK_BOX(fb_auth_dialog->vbox), hbox, TRUE, TRUE, 0);

  gtk_widget_show_all(GTK_WIDGET(fb_auth_dialog));

  ////////////// wait for the user to entrer the validation URL
  gint result;
  gchar *token = NULL;
  const char *replyurl;
  while (TRUE)
  {
    result = gtk_dialog_run (GTK_DIALOG (fb_auth_dialog));
    if (result == GTK_RESPONSE_CANCEL)
      break;
    replyurl = gtk_entry_get_text(GTK_ENTRY(entry));
    if (replyurl == NULL || g_strcmp0(replyurl, "") == 0)
    {
      gtk_message_dialog_format_secondary_markup(GTK_MESSAGE_DIALOG(fb_auth_dialog),
                                                 "%s\n\n%s\n\n<span foreground=\"" MSGCOLOR_RED "\" ><small>%s</small></span>",
                                                 text1, text2, _("please enter the validation url"));
      continue;
    }
    token = fb_extract_token_from_url(replyurl);
    if (token != NULL)//we have a valid token
      break;
    else
      gtk_message_dialog_format_secondary_markup(
            GTK_MESSAGE_DIALOG(fb_auth_dialog),
            "%s\n\n%s%s\n\n<span foreground=\"" MSGCOLOR_RED "\"><small>%s</small></span>",
            text1, text2,
            _("the given url is not valid, it should look like: "),
              FB_WS_BASE_URL"connect/login_success.html?...");
  }
  gtk_widget_destroy(GTK_WIDGET(fb_auth_dialog));

  return token;
}


static void load_account_info_fill(gchar *key, gchar *value, GSList **accountlist)
{
  FBAccountInfo *info = fb_account_info_init();
  info->id = g_strdup(key);

  JsonParser *parser = json_parser_new();
  json_parser_load_from_data(parser, value, strlen(value), NULL);
  JsonNode *root = json_parser_get_root(parser);
  JsonObject *obj = json_node_get_object(root);
  info->token = g_strdup(json_object_get_string_member(obj, "token"));
  info->username = g_strdup(json_object_get_string_member(obj, "username"));
  *accountlist =  g_slist_prepend(*accountlist, info);
  g_object_unref(parser);
}

/**
 * @return a GSList of saved FBAccountInfo
 */
static GSList *load_account_info()
{
  GSList *accountlist = NULL;

  GHashTable *table = dt_pwstorage_get("facebook");
  g_hash_table_foreach(table, (GHFunc) load_account_info_fill, &accountlist);
  return accountlist;
}

static void save_account_info(dt_storage_facebook_gui_data_t *ui, FBAccountInfo *accountinfo)
{
  FBContext *ctx = ui->facebook_api;
  g_return_if_fail(ctx != NULL);

  ///serialize data;
  JsonBuilder *builder = json_builder_new();
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "username");
  json_builder_add_string_value(builder, accountinfo->username);
  json_builder_set_member_name(builder, "token");
  json_builder_add_string_value(builder, accountinfo->token);
  json_builder_end_object(builder);

  JsonNode *node = json_builder_get_root(builder);
  JsonGenerator *generator = json_generator_new();
  json_generator_set_root(generator, node);
  json_generator_set_pretty(generator, FALSE);
  gchar *data = json_generator_to_data(generator, NULL);

  json_node_free(node);
  g_object_unref(generator);
  g_object_unref(builder);

  GHashTable *table = dt_pwstorage_get("facebook");
  g_hash_table_insert(table, accountinfo->id, data);
  dt_pwstorage_set("facebook", table);

  g_hash_table_destroy(table);
}

static void remove_account_info(const gchar *accountid)
{
  GHashTable *table = dt_pwstorage_get("facebook");
  g_hash_table_remove(table, accountid);
  dt_pwstorage_set("facebook", table);
  g_hash_table_destroy(table);
}

static void ui_refresh_users_fill(FBAccountInfo *value, gpointer dataptr)
{
  GtkListStore *liststore = GTK_LIST_STORE(dataptr);
  GtkTreeIter iter;
  gtk_list_store_append(liststore, &iter);
  gtk_list_store_set(liststore, &iter,
                     COMBO_USER_MODEL_NAME_COL, value->username,
                     COMBO_USER_MODEL_TOKEN_COL, value->token,
                     COMBO_USER_MODEL_ID_COL, value->id, -1);
}

static void ui_refresh_users(dt_storage_facebook_gui_data_t *ui)
{
  GSList *accountlist= load_account_info();
  GtkListStore *list_store = GTK_LIST_STORE(gtk_combo_box_get_model(ui->comboBox_username));
  GtkTreeIter iter;

  // reset iter before clearing model to avoid a crash
  gtk_combo_box_set_active_iter(ui->comboBox_username, NULL);
  gtk_list_store_clear(list_store);
  gtk_list_store_append(list_store, &iter);

  if (g_slist_length(accountlist) == 0)
  {
    gtk_list_store_set(list_store, &iter,
                       COMBO_USER_MODEL_NAME_COL, _("new account"),
                       COMBO_USER_MODEL_TOKEN_COL, NULL,
                       COMBO_USER_MODEL_ID_COL, NULL, -1);
  }
  else
  {
    gtk_list_store_set(list_store, &iter,
                       COMBO_USER_MODEL_NAME_COL, _("other account"),
                       COMBO_USER_MODEL_TOKEN_COL, NULL,
                       COMBO_USER_MODEL_ID_COL, NULL,-1);
    gtk_list_store_append(list_store, &iter);
    gtk_list_store_set(list_store, &iter,
                       COMBO_USER_MODEL_NAME_COL, "",
                       COMBO_USER_MODEL_TOKEN_COL, NULL,
                       COMBO_USER_MODEL_ID_COL, NULL,-1);//separator
  }

  g_slist_foreach(accountlist, (GFunc)ui_refresh_users_fill, list_store);
  gtk_combo_box_set_active(ui->comboBox_username, 0);

  g_slist_free_full(accountlist, (GDestroyNotify)fb_account_info_destroy);
  gtk_combo_box_set_row_separator_func(ui->comboBox_username,combobox_separator,ui->comboBox_username,NULL);
}

static void ui_refresh_albums_fill(FBAlbum *album, GtkListStore *list_store)
{
  GtkTreeIter iter;
  gtk_list_store_append(list_store, &iter);
  gtk_list_store_set(list_store, &iter, COMBO_ALBUM_MODEL_NAME_COL, album->name, COMBO_ALBUM_MODEL_ID_COL, album->id, -1);
}

static void ui_refresh_albums(dt_storage_facebook_gui_data_t *ui)
{
  gboolean getlistok;
  GList *albumList = fb_get_album_list(ui->facebook_api, &getlistok);
  if (! getlistok)
  {
    dt_control_log(_("unable to retreive the album list"));
    goto cleanup;
  }

  // reset iter before clearing model to avoid a crash
  gtk_combo_box_set_active_iter(ui->comboBox_album, NULL);
  GtkListStore *model_album = GTK_LIST_STORE(gtk_combo_box_get_model(ui->comboBox_album));
  GtkTreeIter iter;
  gtk_list_store_clear(model_album);
  gtk_list_store_append(model_album, &iter);
  gtk_list_store_set(model_album, &iter, COMBO_ALBUM_MODEL_NAME_COL, _("create new album"), COMBO_ALBUM_MODEL_ID_COL, NULL, -1);
  if (albumList != NULL)
  {
    gtk_list_store_append(model_album, &iter);
    gtk_list_store_set(model_album, &iter, COMBO_ALBUM_MODEL_NAME_COL, "", COMBO_ALBUM_MODEL_ID_COL, NULL, -1); //separator
  }
  g_list_foreach(albumList, (GFunc)ui_refresh_albums_fill, model_album);

  if (albumList != NULL)
    gtk_combo_box_set_active(ui->comboBox_album, 2);
  else
    gtk_combo_box_set_active(ui->comboBox_album, 0);

  gtk_widget_show_all(GTK_WIDGET(ui->comboBox_album));
  g_list_free_full(albumList, (GDestroyNotify)fb_album_destroy);

cleanup:
  return;
}

static gboolean ui_authenticate(dt_storage_facebook_gui_data_t *ui)
{
  if (ui->facebook_api == NULL)
  {
    ui->facebook_api = fb_api_init();
  }

  FBContext *ctx = ui->facebook_api;
  gboolean mustsaveaccount = FALSE;

  gchar *uiselectedaccounttoken = NULL;
  GtkTreeIter iter;
  gtk_combo_box_get_active_iter(ui->comboBox_username, &iter);
  GtkTreeModel *accountModel = gtk_combo_box_get_model(ui->comboBox_username);
  gtk_tree_model_get(accountModel, &iter, 1, &uiselectedaccounttoken, -1);

  if (ctx->token != NULL)
  {
    g_free(ctx->token);
    ctx->token = NULL;
  }
  if (uiselectedaccounttoken != NULL)
  {
    ctx->token = g_strdup(uiselectedaccounttoken);
  }
  //check selected token if we akready have one
  if (ctx->token != NULL && !fb_test_auth_token(ctx))
  {
      g_free(ctx->token);
      ctx->token = NULL;
  }

  if(ctx->token == NULL)
  {
    mustsaveaccount = TRUE;
    ctx->token = facebook_get_user_auth_token(ui);//ask user to log in
  }

  if (ctx->token == NULL)
  {
    return FALSE;
  }
  else
  {
    if (mustsaveaccount)
    {
      FBAccountInfo *accountinfo = fb_get_account_info(ui->facebook_api);
      g_return_val_if_fail(accountinfo != NULL, FALSE);
      save_account_info(ui, accountinfo);

      // reset iter before clearing model to avoid a crash
      gtk_combo_box_set_active_iter(ui->comboBox_username, NULL);
      //add account to user list and select it
      GtkListStore *model =  GTK_LIST_STORE(gtk_combo_box_get_model(ui->comboBox_username));
      GtkTreeIter iter;
      gboolean r;
      gchar *uid;

      gboolean updated = FALSE;

      for (r = gtk_tree_model_get_iter_first (GTK_TREE_MODEL(model), &iter);
           r == TRUE;
           r = gtk_tree_model_iter_next (GTK_TREE_MODEL(model), &iter))
      {
        gtk_tree_model_get (GTK_TREE_MODEL(model), &iter, COMBO_USER_MODEL_ID_COL, &uid, -1);

        if (g_strcmp0(uid, accountinfo->id) == 0)
        {
          gtk_list_store_set(model, &iter, COMBO_USER_MODEL_NAME_COL, accountinfo->username,
                                           COMBO_USER_MODEL_TOKEN_COL, accountinfo->token,
                                           -1);
          updated = TRUE;
          break;
        }
      }

      if (!updated)
      {
        gtk_list_store_append(model, &iter);
        gtk_list_store_set(model, &iter, COMBO_USER_MODEL_NAME_COL, accountinfo->username,
                                         COMBO_USER_MODEL_TOKEN_COL, accountinfo->token,
                                         COMBO_USER_MODEL_ID_COL, accountinfo->id, -1);
      }
      gtk_combo_box_set_active_iter(ui->comboBox_username, &iter);
      //we have to re-set the current token here since ui_combo_username_changed is called
      //on gtk_combo_box_set_active_iter (and thus is reseting the active token)
      ctx->token = g_strdup(accountinfo->token);
      fb_account_info_destroy(accountinfo);
    }
    return TRUE;
  }
}


static void ui_login_clicked(GtkButton *button, gpointer data)
{
  dt_storage_facebook_gui_data_t *ui=(dt_storage_facebook_gui_data_t*)data;
  gtk_widget_set_sensitive(GTK_WIDGET(ui->comboBox_album), FALSE);
  if (ui->connected == FALSE)
  {
    if (ui_authenticate(ui))
    {
      ui_refresh_albums(ui);
      ui->connected = TRUE;
      gtk_button_set_label(ui->button_login, _("logout"));
    }
    else
    {
      gtk_button_set_label(ui->button_login, _("login"));
    }
  }
  else //disconnect user
  {
    if (ui->connected == TRUE && ui->facebook_api->token != NULL)
    {
      GtkTreeModel *model = gtk_combo_box_get_model(ui->comboBox_username);
      GtkTreeIter iter;
      gtk_combo_box_get_active_iter(ui->comboBox_username, &iter);
      gchar *userid;
      gtk_tree_model_get(model, &iter, COMBO_USER_MODEL_ID_COL, &userid, -1);
      remove_account_info(userid);
      gtk_button_set_label(ui->button_login, _("login"));
      ui_refresh_users(ui);
      ui->connected = FALSE;
    }
  }
  gtk_widget_set_sensitive(GTK_WIDGET(ui->comboBox_album), TRUE);
}


static void ui_reset_albums_creation(struct dt_storage_facebook_gui_data_t *ui)
{
  // iter must be reset before nuking the model if we want to avoid crashing.
  gtk_combo_box_set_active_iter(ui->comboBox_album, NULL);
  GtkListStore *model_album = GTK_LIST_STORE(gtk_combo_box_get_model(ui->comboBox_album));
  gtk_list_store_clear(model_album);
  gtk_entry_set_text(ui->entry_album_summary, "");
  gtk_entry_set_text(ui->entry_album_title, "");
  gtk_widget_hide_all(GTK_WIDGET(ui->hbox_album));
}

static void ui_combo_username_changed(GtkComboBox *combo, struct dt_storage_facebook_gui_data_t *ui)
{
  GtkTreeIter iter;
  gchar *token = NULL;
  if (!gtk_combo_box_get_active_iter(combo, &iter))
    return; //ie: list is empty while clearing the combo
  GtkTreeModel *model = gtk_combo_box_get_model(combo);
  gtk_tree_model_get( model, &iter, 1, &token, -1);//get the selected token
  ui->connected = FALSE;
  gtk_button_set_label(ui->button_login, _("login"));
  if (ui->facebook_api->token != NULL)
  g_free(ui->facebook_api->token);
  ui->facebook_api->token = NULL;
  ui_reset_albums_creation(ui);
}

static void ui_combo_album_changed(GtkComboBox *combo, gpointer data)
{
  dt_storage_facebook_gui_data_t *ui = (dt_storage_facebook_gui_data_t*)data;

  GtkTreeIter iter;
  gchar *albumid = NULL;
  if (gtk_combo_box_get_active_iter(combo, &iter))
  {
    GtkTreeModel  *model = gtk_combo_box_get_model( combo );
    gtk_tree_model_get( model, &iter, 1, &albumid, -1);//get the album id
  }

  if (albumid == NULL)
  {
    gtk_widget_set_no_show_all(GTK_WIDGET(ui->hbox_album), FALSE);
    gtk_widget_show_all(GTK_WIDGET(ui->hbox_album));
  }
  else
  {
    gtk_widget_hide(GTK_WIDGET(ui->hbox_album));
  }

}

////////////////////////// darktable library interface

/* plugin name */
const char *name()
{
  return _("facebook webalbum");
}

/* construct widget above */
void gui_init(struct dt_imageio_module_storage_t *self)
{
  self->gui_data = g_malloc0(sizeof(dt_storage_facebook_gui_data_t));
  dt_storage_facebook_gui_data_t *ui = self->gui_data;
  ui->facebook_api = fb_api_init();

  self->widget = gtk_vbox_new(FALSE, 0);

  //create labels
  ui->label_username = GTK_LABEL(gtk_label_new(_("user")));
  ui->label_album = GTK_LABEL(gtk_label_new(_("album")));
  ui->label_album_title = GTK_LABEL(  gtk_label_new( _("title") ) );
  ui->label_album_summary = GTK_LABEL(  gtk_label_new( _("summary") ) );
  ui->label_album_privacy = GTK_LABEL(gtk_label_new(_("privacy")));
  ui->label_status = GTK_LABEL(gtk_label_new(NULL));

  gtk_misc_set_alignment(GTK_MISC(ui->label_username), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(ui->label_album), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(ui->label_album_title), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(ui->label_album_summary), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(ui->label_album_privacy), 0.0, 0.5);

  //create entries
  GtkListStore *model_username  = gtk_list_store_new (COMBO_USER_MODEL_NB_COL, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING); //text, token, id
  ui->comboBox_username = GTK_COMBO_BOX(gtk_combo_box_new_with_model(GTK_TREE_MODEL(model_username)));
  GtkCellRenderer *p_cell = gtk_cell_renderer_text_new ();
  gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (ui->comboBox_username), p_cell, FALSE);
  gtk_cell_layout_set_attributes (GTK_CELL_LAYOUT (ui->comboBox_username), p_cell, "text", 0, NULL);

  ui->entry_album_title = GTK_ENTRY(gtk_entry_new());
  ui->entry_album_summary = GTK_ENTRY(gtk_entry_new());

  dt_gui_key_accel_block_on_focus(GTK_WIDGET(ui->comboBox_username));
  dt_gui_key_accel_block_on_focus(GTK_WIDGET(ui->entry_album_title));
  dt_gui_key_accel_block_on_focus(GTK_WIDGET(ui->entry_album_summary));

  //retreive saved accounts
  ui_refresh_users(ui);

  //////// album list /////////
  GtkWidget *albumlist = gtk_hbox_new(FALSE, 0);
  GtkListStore *model_album = gtk_list_store_new (COMBO_ALBUM_MODEL_NB_COL, G_TYPE_STRING, G_TYPE_STRING); //name, id
  ui->comboBox_album = GTK_COMBO_BOX(gtk_combo_box_new_with_model(GTK_TREE_MODEL(model_album)));
  p_cell = gtk_cell_renderer_text_new ();
  gtk_cell_layout_pack_start(GTK_CELL_LAYOUT (ui->comboBox_album), p_cell, FALSE);
  gtk_cell_layout_set_attributes (GTK_CELL_LAYOUT (ui->comboBox_album), p_cell, "text", 0, NULL);

  gtk_widget_set_sensitive(GTK_WIDGET(ui->comboBox_album), FALSE);
  gtk_combo_box_set_row_separator_func(ui->comboBox_album,combobox_separator,ui->comboBox_album,NULL);
  gtk_box_pack_start(GTK_BOX(albumlist), GTK_WIDGET(ui->comboBox_album), TRUE, TRUE, 0);

  ui->comboBox_privacy= GTK_COMBO_BOX(gtk_combo_box_new_text());
  GtkListStore *list_store = gtk_list_store_new (COMBO_ALBUM_MODEL_NB_COL, G_TYPE_STRING, G_TYPE_INT);
  GtkTreeIter iter;
  gtk_list_store_append(list_store, &iter);
  gtk_list_store_set(list_store, &iter, COMBO_PRIVACY_MODEL_NAME_COL, _("Only Me"), COMBO_PRIVACY_MODEL_VAL_COL, FBALBUM_PRIVACY_SELF, -1);
  gtk_list_store_append(list_store, &iter);
  gtk_list_store_set(list_store, &iter, COMBO_PRIVACY_MODEL_NAME_COL, _("Friends"), COMBO_PRIVACY_MODEL_VAL_COL, FBALBUM_PRIVACY_ALL_FRIENDS, -1);
  gtk_list_store_append(list_store, &iter);
  gtk_list_store_set(list_store, &iter, COMBO_PRIVACY_MODEL_NAME_COL, _("Public"), COMBO_PRIVACY_MODEL_VAL_COL, FBALBUM_PRIVACY_EVERYONE, -1);
  gtk_list_store_append(list_store, &iter);
  gtk_list_store_set(list_store, &iter, COMBO_PRIVACY_MODEL_NAME_COL, _("Friends of Friends"), COMBO_PRIVACY_MODEL_VAL_COL, FBALBUM_PRIVACY_FRIENDS_OF_FRIENDS, -1);

  gtk_combo_box_set_model(ui->comboBox_privacy, GTK_TREE_MODEL(list_store));

  gtk_combo_box_set_active(GTK_COMBO_BOX(ui->comboBox_privacy), 1); // Set default permission to private
  ui->button_login = GTK_BUTTON(gtk_button_new_with_label(_("login")));
  ui->connected = FALSE;

  //pack the ui
  ////the auth box
  GtkWidget *hbox_auth = gtk_hbox_new(FALSE,5);
  GtkWidget *vbox_auth_labels=gtk_vbox_new(FALSE,0);
  GtkWidget *vbox_auth_fields=gtk_vbox_new(FALSE,0);
  gtk_box_pack_start(GTK_BOX(hbox_auth), vbox_auth_labels, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(hbox_auth), vbox_auth_fields, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox_auth), TRUE, FALSE, 2);
  gtk_box_pack_start(GTK_BOX(vbox_auth_labels), GTK_WIDGET(ui->label_username), TRUE, TRUE, 2);
  gtk_box_pack_start(GTK_BOX(vbox_auth_fields), GTK_WIDGET(ui->comboBox_username), TRUE, FALSE, 2);

  gtk_box_pack_start(GTK_BOX(vbox_auth_labels), GTK_WIDGET(gtk_label_new("")), TRUE, TRUE, 2);
  gtk_box_pack_start(GTK_BOX(vbox_auth_fields), GTK_WIDGET(ui->button_login), TRUE, FALSE, 2);

  gtk_box_pack_start(GTK_BOX(vbox_auth_labels), GTK_WIDGET(ui->label_album), TRUE, TRUE, 2);
  gtk_box_pack_start(GTK_BOX(vbox_auth_fields), GTK_WIDGET(albumlist), TRUE, FALSE, 2);

  ////the album creation box
  ui->hbox_album = GTK_BOX(gtk_hbox_new(FALSE,5));
  gtk_widget_set_no_show_all(GTK_WIDGET(ui->hbox_album), TRUE); //hide it by default
  GtkWidget *vbox_album_labels=gtk_vbox_new(FALSE,0);
  GtkWidget *vbox_album_fields=gtk_vbox_new(FALSE,0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(ui->hbox_album), TRUE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX(ui->hbox_album), vbox_album_labels, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(ui->hbox_album), vbox_album_fields, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(vbox_album_labels), GTK_WIDGET(ui->label_album_title), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(vbox_album_fields), GTK_WIDGET(ui->entry_album_title), TRUE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(vbox_album_labels), GTK_WIDGET(ui->label_album_summary), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(vbox_album_fields), GTK_WIDGET(ui->entry_album_summary), TRUE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(vbox_album_labels), GTK_WIDGET(ui->label_album_privacy), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(vbox_album_fields), GTK_WIDGET(ui->comboBox_privacy), TRUE, FALSE, 0);

  //connect buttons to signals
  g_signal_connect(G_OBJECT(ui->button_login), "clicked", G_CALLBACK(ui_login_clicked), (gpointer)ui);
  g_signal_connect(G_OBJECT(ui->comboBox_username), "changed", G_CALLBACK(ui_combo_username_changed), (gpointer)ui);
  g_signal_connect(G_OBJECT(ui->comboBox_album), "changed", G_CALLBACK(ui_combo_album_changed), (gpointer)ui);

}

/* destroy resources */
void gui_cleanup(struct dt_imageio_module_storage_t *self)
{
  dt_storage_facebook_gui_data_t *ui = self->gui_data;
  if (ui->facebook_api != NULL)
    fb_api_destroy(ui->facebook_api);
  g_free(self->gui_data);
}

/* reset options to defaults */
void gui_reset(struct dt_imageio_module_storage_t *self)
{
  //TODO?
}

/* try and see if this format is supported? */
int supported(struct dt_imageio_module_storage_t *self, struct dt_imageio_module_format_t *format)
{
  if( strcmp(format->mime(NULL) ,"image/jpeg") ==  0 ) return 1;
  else if( strcmp(format->mime(NULL) ,"image/png") ==  0 ) return 1;
  return 0;
}

/* this actually does the work */
int store(struct dt_imageio_module_data_t *sdata, const int imgid, dt_imageio_module_format_t *format, dt_imageio_module_data_t *fdata, const int num, const int total, const gboolean high_quality)
{
  gint result = 1;
  dt_storage_facebook_param_t *p = (dt_storage_facebook_param_t*)sdata;

  const char *ext = format->extension(fdata);
  char fname[4096]= {0};
  dt_loc_get_tmp_dir(fname,4096);
  g_strlcat (fname,"/darktable.XXXXXX.",4096);
  g_strlcat(fname,ext,4096);

  gint fd=g_mkstemp(fname);
  if(fd==-1)
  {
    dt_control_log("failed to create temporary image for facebook export");
    return 1;
  }
  close(fd);

  //get metadata
  const dt_image_t *img = dt_image_cache_read_get(darktable.image_cache, imgid);
  char *caption = NULL;
  GList *title = NULL;
  GList *desc = NULL;

  title = dt_metadata_get(img->id, "Xmp.dc.title", NULL);
  if(title != NULL)
  {
    caption = title->data;
  }
  if (caption == NULL)
  {
    desc = dt_metadata_get(img->id, "Xmp.dc.description", NULL);
    if(desc != NULL)
    {
      caption = desc->data;
    }
  }
  dt_image_cache_read_release(darktable.image_cache, img);

  //facebook doesn't allow picture bigger than 960x960 px
  if (fdata->max_height == 0 || fdata->max_height > FB_IMAGE_MAX_SIZE)
    fdata->max_height = FB_IMAGE_MAX_SIZE;
  if (fdata->max_width == 0 || fdata->max_width > FB_IMAGE_MAX_SIZE)
    fdata->max_width = FB_IMAGE_MAX_SIZE;

  if(dt_imageio_export(imgid, fname, format, fdata, high_quality) != 0)
  {
    g_printerr("[facebook] could not export to file: `%s'!\n", fname);
    dt_control_log(_("could not export to file `%s'!"), fname);
    result = 0;
    goto cleanup;
  }

  if (p->facebook_ctx->album_id == NULL)
  {
    if (p->facebook_ctx->album_title == NULL)
    {
      dt_control_log(_("unable to create album, no title provided"));
      result = 0;
      goto cleanup;
    }
    const gchar *album_id = fb_create_album(p->facebook_ctx, p->facebook_ctx->album_title, p->facebook_ctx->album_summary, p->facebook_ctx->album_permission);
    if (album_id == NULL)
    {
      dt_control_log(_("unable to create album"));
      result = 0;
      goto cleanup;
    }
    p->facebook_ctx->album_id = g_strdup(album_id);
  }

  const char *photoid = fb_upload_photo_to_album(p->facebook_ctx, p->facebook_ctx->album_id, fname, caption);
  if (photoid == NULL)
  {
    dt_control_log(_("unable to export photo to webalbum"));
    result = 0;
    goto cleanup;
  }

cleanup:
  unlink( fname );
  g_free( caption );
  if(desc)
  {
    //no need to free desc->data as caption points to it
    g_list_free(desc);
  }

  if (result)
  {
    //this makes sense only if the export was successful
    dt_control_log(_("%d/%d exported to facebook webalbum"), num, total );
  }
  return 0;
}


int finalize_store(struct dt_imageio_module_storage_t *self, dt_imageio_module_data_t *data)
{
  gdk_threads_enter();
  dt_storage_facebook_gui_data_t *ui = (dt_storage_facebook_gui_data_t*)self->gui_data;
  ui_reset_albums_creation(ui);
  ui_refresh_albums(ui);
  gdk_threads_leave();
  return 1;
}


void *get_params(struct dt_imageio_module_storage_t *self, int *size)
{
  *size = sizeof(gint64);
  dt_storage_facebook_gui_data_t *ui = (dt_storage_facebook_gui_data_t*)self->gui_data;
  if(ui->facebook_api == NULL || ui->facebook_api->token == NULL)
  {
    return NULL;
  }
  dt_storage_facebook_param_t *p = (dt_storage_facebook_param_t*)g_malloc0(sizeof(dt_storage_facebook_param_t));
  p->hash = 1;
  p->facebook_ctx = ui->facebook_api;
  int index = gtk_combo_box_get_active(ui->comboBox_album);
  if (index < 0)
  {
    g_free(p);
    return NULL;
  }
  else if (index == 0)
  {
    p->facebook_ctx->album_id = NULL;
    p->facebook_ctx->album_title = g_strdup(gtk_entry_get_text(ui->entry_album_title));
    p->facebook_ctx->album_summary = g_strdup(gtk_entry_get_text(ui->entry_album_summary));
    GtkTreeModel *model = gtk_combo_box_get_model(ui->comboBox_privacy);
    GtkTreeIter iter;
    int permission = -1;
    gtk_combo_box_get_active_iter(ui->comboBox_privacy, &iter);
    gtk_tree_model_get(model, &iter, COMBO_PRIVACY_MODEL_VAL_COL, &permission, -1);
    p->facebook_ctx->album_permission = permission;
  }
  else
  {
    GtkTreeModel *model = gtk_combo_box_get_model(ui->comboBox_album);
    GtkTreeIter iter;
    gchar *albumid = NULL;
    gtk_combo_box_get_active_iter(ui->comboBox_album, &iter);
    gtk_tree_model_get(model, &iter, COMBO_ALBUM_MODEL_ID_COL, &albumid, -1);
    p->facebook_ctx->album_id = g_strdup(albumid);
  }

  //recreate a new context for further usages
  ui->facebook_api = fb_api_init();
  ui->facebook_api->token = g_strdup(p->facebook_ctx->token);
  return p;
}


void free_params(struct dt_imageio_module_storage_t *self, dt_imageio_module_data_t *data)
{
  dt_storage_facebook_param_t *p = (dt_storage_facebook_param_t*)data;
  fb_api_destroy(p->facebook_ctx);
  g_free(p);
}

int set_params(struct dt_imageio_module_storage_t *self, const void *params, const int size)
{
  if(size != sizeof(gint64))
    return 1;
  // gui stuff not updated, as sensitive user data is not stored in the preset.
  // TODO: store name/hash in kwallet/etc module and get encrypted stuff from there!
  return 0;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;