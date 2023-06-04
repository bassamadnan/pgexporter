/*
 * Copyright (C) 2023 Red Hat
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this list
 * of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice, this
 * list of conditions and the following disclaimer in the documentation and/or other
 * materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors may
 * be used to endorse or promote products derived from this software without specific
 * prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
 * TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* pgexporter */
#include <pgexporter.h>
#include <logging.h>
#include <memory.h>
#include <message.h>
#include <network.h>
#include <prometheus.h>
#include <queries.h>
#include <query_alts.h>
#include <security.h>
#include <shmem.h>
#include <utils.h>

/* system */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

#define CHUNK_SIZE 32768

#define PAGE_UNKNOWN 0
#define PAGE_HOME    1
#define PAGE_METRICS 2
#define BAD_REQUEST  3

#define MAX_ARR_LENGTH 256
#define NUMBER_OF_HISTOGRAM_COLUMNS 4

/**
 * This is a linked list of queries with the data received from the server
 * as well as the query sent to the server and other meta data.
 **/
typedef struct query_list
{
   struct query* query;
   struct query_list* next;
   query_alts_t* query_alt;
   char tag[MISC_LENGTH];
   int sort_type;
} query_list_t;

/**
 * This is one of the nodes of a linked list of a column entry.
 *
 * Since columns are the fundamental unit in a metric and since
 * due to different versions of servers, each query might have
 * a variable structure, dividing each query into its constituent
 * columns is needed.
 *
 * Then each received tuple can have their individual column values
 * appended to the suitable linked list of `column_node_t`.
 **/
typedef struct column_node
{
   char* data;
   struct tuple* tuple;
   struct column_node* next;
} column_node_t;

/**
 * It stores the metadata of a `column_node_t` linked list.
 * Meant to be used as part of an array
 **/
typedef struct column_store
{
   column_node_t* columns;
   column_node_t* last_column;
   char tag[MISC_LENGTH];
   int type;
   char name[MISC_LENGTH];
   int sort_type;
} column_store_t;

static int resolve_page(struct message* msg);
static int unknown_page(int client_fd);
static int home_page(int client_fd);
static int metrics_page(int client_fd);
static int bad_request(int client_fd);

static bool collector_pass(const char* collector);

static void add_column_to_store(column_store_t* store, int n_store, char* data, int sort_type, struct tuple* current);

static void general_information(int client_fd);
static void core_information(int client_fd);
static void extension_information(int client_fd);
static void extension_function(int client_fd, char* function, char* description, char* type);
static void server_information(int client_fd);
static void version_information(int client_fd);
static void uptime_information(int client_fd);
static void primary_information(int client_fd);
static void settings_information(int client_fd);
static void custom_metrics(int client_fd); // Handles custom metrics provided in YAML format, both internal and external

static void append_help_info(char** data, char* tag, char* name, char* description);
static void append_type_info(char** data, char* tag, char* name, int typeId);

static void handle_histogram(column_store_t* store, int* n_store, query_list_t* temp);
static void handle_gauge_counter(column_store_t* store, int* n_store, query_list_t* temp);

static int send_chunk(int client_fd, char* data);
static int parse_list(char* list_str, char** strs, int* n_strs);

static char* get_value(char* tag, char* name, char* val);
static char* safe_prometheus_key(char* key);

static bool is_metrics_cache_configured(void);
static bool is_metrics_cache_valid(void);
static bool metrics_cache_append(char* data);
static bool metrics_cache_finalize(void);
static size_t metrics_cache_size_to_alloc(void);
static void metrics_cache_invalidate(void);

void
pgexporter_prometheus(int client_fd)
{
   int status;
   int page;
   struct message* msg = NULL;
   struct configuration* config;

   pgexporter_start_logging();
   pgexporter_memory_init();

   config = (struct configuration*)shmem;

   status = pgexporter_read_timeout_message(NULL, client_fd, config->authentication_timeout, &msg);
   if (status != MESSAGE_STATUS_OK)
   {
      goto error;
   }

   page = resolve_page(msg);

   if (page == PAGE_HOME)
   {
      home_page(client_fd);
   }
   else if (page == PAGE_METRICS)
   {
      metrics_page(client_fd);
   }
   else if (page == PAGE_UNKNOWN)
   {
      unknown_page(client_fd);
   }
   else
   {
      bad_request(client_fd);
   }

   pgexporter_disconnect(client_fd);

   pgexporter_memory_destroy();
   pgexporter_stop_logging();

   exit(0);

error:

   pgexporter_disconnect(client_fd);

   pgexporter_memory_destroy();
   pgexporter_stop_logging();

   exit(1);
}

void
pgexporter_prometheus_reset(void)
{
   signed char cache_is_free;
   struct prometheus_cache* cache;

   cache = (struct prometheus_cache*)prometheus_cache_shmem;

retry_cache_locking:
   cache_is_free = STATE_FREE;
   if (atomic_compare_exchange_strong(&cache->lock, &cache_is_free, STATE_IN_USE))
   {
      metrics_cache_invalidate();

      atomic_store(&cache->lock, STATE_FREE);
   }
   else
   {
      /* Sleep for 1ms */
      SLEEP_AND_GOTO(1000000L, retry_cache_locking);
   }
}

static int
resolve_page(struct message* msg)
{
   char* from = NULL;
   int index;

   if (msg->length < 3 || strncmp((char*)msg->data, "GET", 3) != 0)
   {
      pgexporter_log_debug("Prometheus: Not a GET request");
      return BAD_REQUEST;
   }

   index = 4;
   from = (char*)msg->data + index;

   while (pgexporter_read_byte(msg->data + index) != ' ')
   {
      index++;
   }

   pgexporter_write_byte(msg->data + index, '\0');

   if (strcmp(from, "/") == 0 || strcmp(from, "/index.html") == 0)
   {
      return PAGE_HOME;
   }
   else if (strcmp(from, "/metrics") == 0)
   {
      return PAGE_METRICS;
   }

   return PAGE_UNKNOWN;
}

static int
unknown_page(int client_fd)
{
   char* data = NULL;
   time_t now;
   char time_buf[32];
   int status;
   struct message msg;

   memset(&msg, 0, sizeof(struct message));
   memset(&data, 0, sizeof(data));

   now = time(NULL);

   memset(&time_buf, 0, sizeof(time_buf));
   ctime_r(&now, &time_buf[0]);
   time_buf[strlen(time_buf) - 1] = 0;

   data = pgexporter_vappend(data, 4,
                             "HTTP/1.1 403 Forbidden\r\n",
                             "Date: ",
                             &time_buf[0],
                             "\r\n"
                             );

   msg.kind = 0;
   msg.length = strlen(data);
   msg.data = data;

   status = pgexporter_write_message(NULL, client_fd, &msg);

   free(data);

   return status;
}

static int
home_page(int client_fd)
{
   char* data = NULL;
   time_t now;
   char time_buf[32];
   int status;
   struct message msg;
   struct configuration* config;

   config = (struct configuration*) shmem;

   memset(&msg, 0, sizeof(struct message));
   memset(&data, 0, sizeof(data));

   now = time(NULL);

   memset(&time_buf, 0, sizeof(time_buf));
   ctime_r(&now, &time_buf[0]);
   time_buf[strlen(time_buf) - 1] = 0;

   data = pgexporter_vappend(data, 7,
                             "HTTP/1.1 200 OK\r\n",
                             "Content-Type: text/html; charset=utf-8\r\n",
                             "Date: ",
                             &time_buf[0],
                             "\r\n",
                             "Transfer-Encoding: chunked\r\n",
                             "\r\n"
                             );

   msg.kind = 0;
   msg.length = strlen(data);
   msg.data = data;

   status = pgexporter_write_message(NULL, client_fd, &msg);
   if (status != MESSAGE_STATUS_OK)
   {
      goto done;
   }

   free(data);
   data = NULL;

   data = pgexporter_vappend(data, 12,
                             "<html>\n",
                             "<head>\n",
                             "  <title>pgexporter</title>\n",
                             "</head>\n",
                             "<body>\n",
                             "  <h1>pgexporter</h1>\n",
                             "  Prometheus exporter for PostgreSQL\n",
                             "  <p>\n",
                             "  <a href=\"/metrics\">Metrics</a>\n",
                             "  <p>\n",
                             "  Support for\n",
                             "  <ul>\n"
                             );

   if (config->number_of_metrics == 0)
   {
      data = pgexporter_vappend(data, 7,
                                "  <li>pg_database</li>\n",
                                "  <li>pg_locks</li>\n",
                                "  <li>pg_replication_slots</li>\n",
                                "  <li>pg_settings</li>\n",
                                "  <li>pg_stat_bgwriter</li>\n",
                                "  <li>pg_stat_database</li>\n",
                                "  <li>pg_stat_database_conflicts</li>\n"
                                );
   }
   else
   {
      for (int i = 0; i < config->number_of_metrics; i++)
      {
         data = pgexporter_vappend(data, 3,
                                   "  <li>",
                                   config->prometheus[i].tag,
                                   "</li>\n"
                                   );
      }
   }

   data = pgexporter_vappend(data, 5,
                             "  </ul>\n",
                             "  <p>\n",
                             "  <a href=\"https://pgexporter.github.io/\">pgexporter.github.io/</a>\n",
                             "</body>\n",
                             "</html>\n"
                             );

   send_chunk(client_fd, data);
   free(data);
   data = NULL;

   /* Footer */
   data = pgexporter_append(data, "0\r\n\r\n");

   msg.kind = 0;
   msg.length = strlen(data);
   msg.data = data;

   status = pgexporter_write_message(NULL, client_fd, &msg);

done:
   if (data != NULL)
   {
      free(data);
   }

   return status;
}

static int
metrics_page(int client_fd)
{
   char* data = NULL;
   time_t now;
   char time_buf[32];
   int status;
   struct message msg;
   struct prometheus_cache* cache;
   signed char cache_is_free;

   cache = (struct prometheus_cache*)prometheus_cache_shmem;

   memset(&msg, 0, sizeof(struct message));

retry_cache_locking:
   cache_is_free = STATE_FREE;
   if (atomic_compare_exchange_strong(&cache->lock, &cache_is_free, STATE_IN_USE))
   {
      // can serve the message out of cache?
      if (is_metrics_cache_configured() && is_metrics_cache_valid())
      {
         // serve the message directly out of the cache
         pgexporter_log_debug("Serving metrics out of cache (%d/%d bytes valid until %lld)",
                              strlen(cache->data),
                              cache->size,
                              cache->valid_until);

         msg.kind = 0;
         msg.length = strlen(cache->data);
         msg.data = cache->data;

         status = pgexporter_write_message(NULL, client_fd, &msg);
         if (status != MESSAGE_STATUS_OK)
         {
            goto error;
         }
      }
      else
      {
         // build the message without the cache
         metrics_cache_invalidate();

         now = time(NULL);

         memset(&time_buf, 0, sizeof(time_buf));
         ctime_r(&now, &time_buf[0]);
         time_buf[strlen(time_buf) - 1] = 0;

         data = pgexporter_vappend(data, 5,
                                   "HTTP/1.1 200 OK\r\n",
                                   "Content-Type: text/plain; version=0.0.1; charset=utf-8\r\n",
                                   "Date: ",
                                   &time_buf[0],
                                   "\r\n"
                                   );
         metrics_cache_append(data);  // cache here to avoid the chunking for the cache
         data = pgexporter_vappend(data, 2,
                                   "Transfer-Encoding: chunked\r\n",
                                   "\r\n"
                                   );

         msg.kind = 0;
         msg.length = strlen(data);
         msg.data = data;

         status = pgexporter_write_message(NULL, client_fd, &msg);
         if (status != MESSAGE_STATUS_OK)
         {
            goto error;
         }

         free(data);
         data = NULL;

         pgexporter_open_connections();

         /* General Metric Collector */
         general_information(client_fd);
         core_information(client_fd);
         server_information(client_fd);
         version_information(client_fd);
         uptime_information(client_fd);
         primary_information(client_fd);
         settings_information(client_fd);
         extension_information(client_fd);

         custom_metrics(client_fd);

         pgexporter_close_connections();

         /* Footer */
         data = pgexporter_append(data, "0\r\n\r\n");

         msg.kind = 0;
         msg.length = strlen(data);
         msg.data = data;

         status = pgexporter_write_message(NULL, client_fd, &msg);
         if (status != MESSAGE_STATUS_OK)
         {
            goto error;
         }

         metrics_cache_finalize();
      }

      // free the cache
      atomic_store(&cache->lock, STATE_FREE);
   }
   else
   {
      /* Sleep for 1ms */
      SLEEP_AND_GOTO(1000000L, retry_cache_locking);
   }

   free(data);

   return 0;

error:

   free(data);

   return 1;
}

static int
bad_request(int client_fd)
{
   char* data = NULL;
   time_t now;
   char time_buf[32];
   int status;
   struct message msg;

   memset(&msg, 0, sizeof(struct message));
   memset(&data, 0, sizeof(data));

   now = time(NULL);

   memset(&time_buf, 0, sizeof(time_buf));
   ctime_r(&now, &time_buf[0]);
   time_buf[strlen(time_buf) - 1] = 0;

   data = pgexporter_vappend(data, 4,
                             "HTTP/1.1 400 Bad Request\r\n",
                             "Date: ",
                             &time_buf[0],
                             "\r\n"
                             );

   msg.kind = 0;
   msg.length = strlen(data);
   msg.data = data;

   status = pgexporter_write_message(NULL, client_fd, &msg);

   free(data);

   return status;
}

static bool
collector_pass(const char* collector)
{
   struct configuration* config = NULL;

   config = (struct configuration*)shmem;

   if (config->number_of_collectors == 0)
   {
      return true;
   }

   for (int i = 0; i < config->number_of_collectors; i++)
   {
      if (!strcmp(config->collectors[i], collector))
      {
         return true;
      }
   }

   return false;
}

static void
general_information(int client_fd)
{
   char* data = NULL;

   data = pgexporter_vappend(data, 4,
                             "#HELP pgexporter_state The state of pgexporter\n",
                             "#TYPE pgexporter_state gauge\n",
                             "pgexporter_state 1\n",
                             "\n"
                             );

   if (data != NULL)
   {
      send_chunk(client_fd, data);
      metrics_cache_append(data);
      free(data);
      data = NULL;
   }
}

static void
server_information(int client_fd)
{
   char* data = NULL;
   struct configuration* config;

   config = (struct configuration*)shmem;

   data = pgexporter_vappend(data, 2,
                             "#HELP pgexporter_postgresql_active The state of PostgreSQL\n",
                             "#TYPE pgexporter_postgresql_active gauge\n"
                             );

   for (int server = 0; server < config->number_of_servers; server++)
   {
      data = pgexporter_vappend(data, 3,
                                "pgexporter_postgresql_active{server=\"",
                                &config->servers[server].name[0],
                                "\"} "
                                );
      if (config->servers[server].fd != -1)
      {
         data = pgexporter_append(data, "1");
      }
      else
      {
         data = pgexporter_append(data, "0");
      }
      data = pgexporter_append(data, "\n");
   }
   data = pgexporter_append(data, "\n");

   if (data != NULL)
   {
      send_chunk(client_fd, data);
      metrics_cache_append(data);
      free(data);
      data = NULL;
   }
}

static void
version_information(int client_fd)
{
   int ret;
   int server;
   char* data = NULL;
   struct query* all = NULL;
   struct query* query = NULL;
   struct tuple* current = NULL;
   struct configuration* config;

   config = (struct configuration*)shmem;

   for (server = 0; server < config->number_of_servers; server++)
   {
      if (config->servers[server].fd != -1)
      {
         ret = pgexporter_query_version(server, &query);
         if (ret == 0)
         {
            all = pgexporter_merge_queries(all, query, SORT_NAME);
         }
         query = NULL;
      }
   }

   if (all != NULL)
   {
      current = all->tuples;
      if (current != NULL)
      {
         data = pgexporter_vappend(data, 2,
                                   "#HELP pgexporter_postgresql_version The PostgreSQL version\n",
                                   "#TYPE pgexporter_postgresql_version gauge\n"
                                   );

         server = 0;

         while (current != NULL)
         {

            data = pgexporter_vappend(data, 6,
                                      "pgexporter_postgresql_version{server=\"",
                                      &config->servers[server].name[0],
                                      "\",version=\"",
                                      safe_prometheus_key(pgexporter_get_column(0, current)),
                                      "\"} ",
                                      "1\n"
                                      );

            server++;
            current = current->next;
         }

         data = pgexporter_append(data, "\n");

         if (data != NULL)
         {
            send_chunk(client_fd, data);
            metrics_cache_append(data);
            free(data);
            data = NULL;
         }
      }
   }

   pgexporter_free_query(all);
}

static void
uptime_information(int client_fd)
{
   int ret;
   int server;
   char* data = NULL;
   struct query* all = NULL;
   struct query* query = NULL;
   struct tuple* current = NULL;
   struct configuration* config;

   config = (struct configuration*)shmem;

   for (server = 0; server < config->number_of_servers; server++)
   {
      if (config->servers[server].fd != -1)
      {
         ret = pgexporter_query_uptime(server, &query);
         if (ret == 0)
         {
            all = pgexporter_merge_queries(all, query, SORT_NAME);
         }
         query = NULL;
      }
   }

   if (all != NULL)
   {
      current = all->tuples;
      if (current != NULL)
      {
         data = pgexporter_vappend(data, 2,
                                   "#HELP pgexporter_postgresql_uptime The PostgreSQL uptime in seconds\n",
                                   "#TYPE pgexporter_postgresql_uptime counter\n"
                                   );

         server = 0;

         while (current != NULL)
         {
            data = pgexporter_vappend(data, 5,
                                      "pgexporter_postgresql_uptime{server=\"",
                                      &config->servers[server].name[0],
                                      "\"} ",
                                      safe_prometheus_key(pgexporter_get_column(0, current)),
                                      "\n"
                                      );

            server++;
            current = current->next;
         }

         data = pgexporter_append(data, "\n");

         if (data != NULL)
         {
            send_chunk(client_fd, data);
            metrics_cache_append(data);
            free(data);
            data = NULL;
         }
      }
   }

   pgexporter_free_query(all);
}

static void
primary_information(int client_fd)
{
   int ret;
   int server;
   char* data = NULL;
   struct query* all = NULL;
   struct query* query = NULL;
   struct tuple* current = NULL;
   struct configuration* config;

   config = (struct configuration*)shmem;

   for (server = 0; server < config->number_of_servers; server++)
   {
      if (config->servers[server].fd != -1)
      {
         ret = pgexporter_query_primary(server, &query);
         if (ret == 0)
         {
            all = pgexporter_merge_queries(all, query, SORT_NAME);
         }
         query = NULL;
      }
   }

   if (all != NULL)
   {
      current = all->tuples;
      if (current != NULL)
      {
         data = pgexporter_vappend(data, 2,
                                   "#HELP pgexporter_postgresql_primary Is the PostgreSQL instance the primary\n",
                                   "#TYPE pgexporter_postgresql_primary gauge\n"
                                   );

         server = 0;

         while (current != NULL)
         {
            data = pgexporter_vappend(data, 3,
                                      "pgexporter_postgresql_primary{server=\"",
                                      &config->servers[server].name[0],
                                      "\"} "
                                      );

            if (!strcmp("t", pgexporter_get_column(0, current)))
            {
               data = pgexporter_append(data, "1");
            }
            else
            {
               data = pgexporter_append(data, "0");
            }
            data = pgexporter_append(data, "\n");

            server++;
            current = current->next;
         }

         data = pgexporter_append(data, "\n");

         if (data != NULL)
         {
            send_chunk(client_fd, data);
            metrics_cache_append(data);
            free(data);
            data = NULL;
         }
      }
   }

   pgexporter_free_query(all);
}

static void
core_information(int client_fd)
{
   char* data = NULL;

   data = pgexporter_vappend(data, 6,
                             "#HELP pgexporter_version The pgexporter version\n",
                             "#TYPE pgexporter_version counter\n",
                             "pgexporter_version{pgexporter_version=\"",
                             VERSION,
                             "\"} 1\n",
                             "\n"
                             );

   if (data != NULL)
   {
      send_chunk(client_fd, data);
      metrics_cache_append(data);
      free(data);
      data = NULL;
   }
}

static void
extension_information(int client_fd)
{
   struct query* query = NULL;
   struct tuple* tuple = NULL;
   struct configuration* config;

   config = (struct configuration*)shmem;

   /* Expose only if default or specified */
   if (!collector_pass("extension"))
   {
      return;
   }

   for (int server = 0; query == NULL && server < config->number_of_servers; server++)
   {
      if (config->servers[server].extension && config->servers[server].fd != -1)
      {
         pgexporter_query_get_functions(server, &query);

         if (query == NULL)
         {
            config->servers[server].extension = false;
            continue;
         }
      }
   }

   if (query != NULL)
   {
      tuple = query->tuples;

      while (tuple != NULL)
      {
         if (!strcmp(tuple->data[1], "f") || !strcmp(tuple->data[1], "false"))
         {
            if (strcmp(tuple->data[0], "pgexporter_get_functions"))
            {
               extension_function(client_fd, tuple->data[0], tuple->data[2], tuple->data[3]);
            }
         }

         tuple = tuple->next;
      }

      pgexporter_free_query(query);
      query = NULL;
   }
}

static void
extension_function(int client_fd, char* function, char* description, char* type)
{
   char* data = NULL;
   bool header = false;
   char* sql = NULL;
   struct query* query = NULL;
   struct tuple* tuple = NULL;
   struct configuration* config;

   config = (struct configuration*)shmem;

   for (int server = 0; server < config->number_of_servers; server++)
   {
      if (config->servers[server].extension && config->servers[server].fd != -1)
      {
         sql = pgexporter_vappend(sql, 3,
                                  "SELECT * FROM ",
                                  function,
                                  "();"
                                  );

         pgexporter_query_execute(server, sql, "pgexporter_ext", &query);

         if (query == NULL)
         {
            config->servers[server].extension = false;
            continue;
         }

         if (!header)
         {
            data = pgexporter_vappend(data, 10,
                                      "#HELP ",
                                      function,
                                      " ",
                                      description,
                                      "\n",
                                      "#TYPE ",
                                      function,
                                      " ",
                                      type,
                                      "\n"
                                      );

            header = true;
         }

         tuple = query->tuples;

         while (tuple != NULL)
         {
            data = pgexporter_vappend(data, 4,
                                      function,
                                      "{server=\"",
                                      &config->servers[server].name[0],
                                      "\""
                                      );

            if (query->number_of_columns > 0)
            {
               data = pgexporter_append(data, ", ");
            }

            for (int col = 0; col < query->number_of_columns; col++)
            {
               data = pgexporter_vappend(data, 4,
                                         query->names[col],
                                         "=\"",
                                         tuple->data[col],
                                         "\""
                                         );

               if (col < query->number_of_columns - 1)
               {
                  data = pgexporter_append(data, ", ");
               }
            }

            data = pgexporter_append(data, "} 1\n");

            tuple = tuple->next;
         }

         free(sql);
         sql = NULL;

         pgexporter_free_query(query);
         query = NULL;
      }
   }

   if (header)
   {
      data = pgexporter_append(data, "\n");
   }

   if (data != NULL)
   {
      send_chunk(client_fd, data);
      metrics_cache_append(data);
      free(data);
      data = NULL;
   }
}

static void
settings_information(int client_fd)
{
   int ret;
   char* data = NULL;
   struct query* all = NULL;
   struct query* query = NULL;
   struct tuple* current = NULL;
   struct configuration* config;

   config = (struct configuration*)shmem;

   /* Expose only if default or specified */
   if (!collector_pass("settings"))
   {
      return;
   }

   for (int server = 0; server < config->number_of_servers; server++)
   {
      if (config->servers[server].fd != -1)
      {
         ret = pgexporter_query_settings(server, &query);
         if (ret == 0)
         {
            all = pgexporter_merge_queries(all, query, SORT_DATA0);
         }
         query = NULL;
      }
   }

   if (all != NULL)
   {
      current = all->tuples;
      while (current != NULL)
      {
         data = pgexporter_vappend(data, 12,
                                   "#HELP pgexporter_",
                                   &all->tag[0],
                                   "_",
                                   safe_prometheus_key(pgexporter_get_column(0, current)),
                                   " ",
                                   safe_prometheus_key(pgexporter_get_column(2, current)),
                                   "\n",
                                   "#TYPE pgexporter_",
                                   &all->tag[0],
                                   "_",
                                   safe_prometheus_key(pgexporter_get_column(0, current)),
                                   " gauge\n"
                                   );

data:
         data = pgexporter_vappend(data, 9,
                                   "pgexporter_",
                                   &all->tag[0],
                                   "_",
                                   safe_prometheus_key(pgexporter_get_column(0, current)),
                                   "{server=\"",
                                   &config->servers[current->server].name[0],
                                   "\"} ",
                                   get_value(&all->tag[0], pgexporter_get_column(0, current), pgexporter_get_column(1, current)),
                                   "\n"
                                   );

         if (current->next != NULL && !strcmp(pgexporter_get_column(0, current), pgexporter_get_column(0, current->next)))
         {
            current = current->next;
            goto data;
         }

         if (data != NULL)
         {
            data = pgexporter_append(data, "\n");

            send_chunk(client_fd, data);
            metrics_cache_append(data);
            free(data);
            data = NULL;
         }

         current = current->next;
      }
   }

   if (data != NULL)
   {
      send_chunk(client_fd, data);
      metrics_cache_append(data);
      free(data);
      data = NULL;
   }

   pgexporter_free_query(all);
}

static void
custom_metrics(int client_fd)
{

   struct configuration* config = NULL;
   char* data = NULL;

   config = (struct configuration*)shmem;

   query_list_t* q_list = NULL;
   query_list_t* temp = q_list;

   // Iterate through each metric to send its query to PostgreSQL server
   for (int i = 0; i < config->number_of_metrics; i++)
   {
      int err = false;
      struct prometheus* prom = &config->prometheus[i];

      /* Expose only if default or specified */
      if (!collector_pass(prom->collector))
      {
         continue;
      }

      // Iterate through each server and send appropriate query to PostgreSQL server
      for (int server = 0; server < config->number_of_servers; server++)
      {
         if (config->servers[server].fd == -1)
         {
            /* Skip */
            continue;
         }

         if ((prom->server_query_type == SERVER_QUERY_PRIMARY && config->servers[server].state != SERVER_PRIMARY) ||
             (prom->server_query_type == SERVER_QUERY_REPLICA && config->servers[server].state != SERVER_REPLICA))
         {
            /* Skip */
            continue;
         }

         // Setting Temp's value
         query_list_t* next = malloc(sizeof(query_list_t));
         memset(next, 0, sizeof(query_list_t));
         if (!err)
         {
            if (!q_list)
            {
               q_list = next;
               temp = q_list;
            }
            else if (temp->query)
            {
               temp->next = next;
               temp = next;
            }
         }
         else
         {
            // Free node with previous data if previous query had an error while executing
            pgexporter_free_query(temp->query);
            if (temp)
            {
               temp->query_alt = NULL;
               free(temp);
            }

            temp = next;
         }

         query_alts_t* query_alt = pgexporter_get_query_alt(prom->root, server);

         if (!query_alt)
         {
            /* Skip */
            continue;
         }

         /* Names */
         char** names = malloc(query_alt->n_columns * sizeof(char*));
         for (int j = 0; j < query_alt->n_columns; j++)
         {
            names[j] = query_alt->columns[j].name;
         }
         memcpy(temp->tag, prom->tag, MISC_LENGTH);
         temp->query_alt = query_alt;

         // Gather all the queries in a linked list, with each query's result (linked list of tuples in it) as a node.
         if (query_alt->is_histogram)
         {
            err = pgexporter_custom_query(server, query_alt->query, prom->tag, -1, NULL, &temp->query);
            temp->sort_type = prom->sort_type;
         }
         else
         {
            err = pgexporter_custom_query(server, query_alt->query, prom->tag, query_alt->n_columns, names, &temp->query);
            temp->sort_type = prom->sort_type;
         }

         free(names);
         names = NULL;
      }
   }

   /* Tuples */
   temp = q_list;
   column_store_t store[MISC_LENGTH] = {0};
   int n_store = 0;

   while (temp)
   {
      if (temp->query_alt->is_histogram)
      {
         handle_histogram(store, &n_store, temp);
      }
      else
      {
         handle_gauge_counter(store, &n_store, temp);
      }

      temp = temp->next;
   }

   for (int i = 0; i < n_store; i++)
   {
      column_node_t* temp = store[i].columns,
                   * last = NULL;

      while (temp)
      {
         data = pgexporter_append(data, temp->data);
         last = temp;
         temp = temp->next;

         // Free it
         free(last->data);
         free(last);
      }
      data = pgexporter_append(data, "\n");
   }

   if (data)
   {
      send_chunk(client_fd, data);
      metrics_cache_append(data);
      free(data);
      data = NULL;
   }

   temp = q_list;
   query_list_t* last = NULL;
   while (temp)
   {

      pgexporter_free_query(temp->query);
      // temp->query_alt // Not freed here, but when program ends
      last = temp;
      temp = temp->next;
      free(last);
   }
}

static int
parse_list(char* list_str, char** strs, int* n_strs)
{
   int idx = 0;
   char* data = NULL;
   char* p = NULL;
   int len = strlen(list_str);

   /**
    * If the `list_str` is `{c1,c2,c3,...,cn}`, and if the `strlen(list_str)`
    * is `x`, then it takes `x + 1` bytes in memory including the null character.
    *
    * `data` will have `list_str` without the first and last bracket (so `data` will
    * just be `c1,c2,c3,...,cn`) and thus `strlen(data)` will be `x - 2`, and
    * so will take `x - 1` bytes in memory including the null character.
    */
   data = (char*) malloc((len - 1) * sizeof(char));
   memset(data, 0, (len - 1) * sizeof(char));

   /**
    * If list_str is `{c1,c2,c3,...,cn}`, then and if `len(list_str)` is `len`
    * then this starts from `c1`, and goes for `len - 2`, so till `cn`, so the
    * `data` string becomes `c1,c2,c3,...,cn`
    */
   strncpy(data, list_str + 1, len - 2);

   p = strtok(data, ",");
   while (p)
   {
      strs[idx] = NULL;
      strs[idx] = pgexporter_append(strs[idx], p);
      idx++;
      p = strtok(NULL, ",");
   }

   *n_strs = idx;
   free(data);
   return 0;
}

static void
add_column_to_store(column_store_t* store, int store_idx, char* data, int sort_type, struct tuple* current)
{
   column_node_t* new_node = malloc(sizeof(column_node_t));
   memset(new_node, 0, sizeof(column_node_t));

   new_node->data = data;
   new_node->tuple = current;

   if (!store[store_idx].columns)
   {
      store[store_idx].columns = new_node;
      store[store_idx].last_column = new_node;
      return;
   }

   if (sort_type == SORT_DATA0)
   {
      // SORT_DATA0 means sorting according to the first data (data[0]) in a tuple.
      // Usually it is the application/database column, so tuples with same such column values
      // are grouped together.
      column_node_t* temp = store[store_idx].columns;

      // first node is for help/type info
      if (!temp->next)
      {
         temp->next = new_node;
         store[store_idx].last_column = new_node;
      }
      else
      {
         while (temp->next)
         {
            if (!strcmp(temp->next->tuple->data[0], current->data[0]))
            {
               break;
            }
            temp = temp->next;
         }

         if (!temp->next)
         {
            temp->next = new_node;
            store[store_idx].last_column = new_node;
         }
         else
         {
            new_node->next = temp->next;
            temp->next = new_node;
         }
      }

   }
   else
   {
      // Current can be null for SORT_NAME
      // Default sort as SORT_NAME
      store[store_idx].last_column->next = new_node;
      store[store_idx].last_column = new_node;
   }
}

static void
handle_histogram(column_store_t* store, int* n_store, query_list_t* temp)
{
   char* data = NULL;
   struct configuration* config;
   int n_bounds = 0;
   int n_buckets = 0;
   char* bounds_arr[MAX_ARR_LENGTH] = {0};
   char* buckets_arr[MAX_ARR_LENGTH] = {0};
   int idx = 0;

   config = (struct configuration*)shmem;

   int h_idx = 0;
   for (; h_idx < temp->query_alt->n_columns; h_idx++)
   {
      if (temp->query_alt->columns[h_idx].type == HISTOGRAM_TYPE)
      {
         break;
      }
   }

   struct tuple* tp = temp->query->tuples;

   if (!tp)
   {
      return;
   }

   char* names[4] = {0};

   /* generate column names X_sum, X_count, X, X_bucket*/
   names[0] = pgexporter_vappend(names[0], 2,
                                 temp->query_alt->columns[h_idx].name,
                                 "_sum"
                                 );
   names[1] = pgexporter_vappend(names[1], 2,
                                 temp->query_alt->columns[h_idx].name,
                                 "_count"
                                 );
   names[2] = pgexporter_vappend(names[2], 1,
                                 temp->query_alt->columns[h_idx].name
                                 );
   names[3] = pgexporter_vappend(names[3], 2,
                                 temp->query_alt->columns[h_idx].name,
                                 "_bucket"
                                 );

   for (; idx < *n_store; idx++)
   {
      if (store[idx].type == HISTOGRAM_TYPE &&
          store[idx].sort_type == temp->sort_type &&
          !strcmp(store[idx].tag, temp->tag) &&
          !strcmp(store[idx].name, temp->query_alt->columns[h_idx].name))
      {
         break;
      }
   }

append:
   if (idx < (*n_store))
   {
      struct tuple* current = temp->query->tuples;

      while (current)
      {
         data = NULL;

         /* bucket */
         char* bounds_str = pgexporter_get_column_by_name(names[2], temp->query, current);
         parse_list(bounds_str, bounds_arr, &n_bounds);

         char* buckets_str = pgexporter_get_column_by_name(names[3], temp->query, current);
         parse_list(buckets_str, buckets_arr, &n_buckets);

         for (int i = 0; i < n_bounds; i++)
         {
            data = pgexporter_vappend(data, 8,
                                      "pgexporter_",
                                      temp->tag,
                                      "_bucket{le=\"",
                                      bounds_arr[i],
                                      "\",",
                                      "server=\"",
                                      &config->servers[current->server].name[0],
                                      "\""
                                      );

            for (int j = 0; j < h_idx; j++)
            {
               data = pgexporter_vappend(data, 5,
                                         ",",
                                         temp->query_alt->columns[j].name,
                                         "=\"",
                                         safe_prometheus_key(pgexporter_get_column(j, current)),
                                         "\""
                                         );
            }

            data = pgexporter_vappend(data, 3,
                                      "} ",
                                      buckets_arr[i],
                                      "\n"
                                      );
         }

         data = pgexporter_vappend(data, 6,
                                   "pgexporter_",
                                   temp->tag,
                                   "_bucket{le=\"+Inf\",",
                                   "server=\"",
                                   &config->servers[current->server].name[0],
                                   "\""
                                   );

         for (int j = 0; j < h_idx; j++)
         {
            data = pgexporter_vappend(data, 5,
                                      ",",
                                      temp->query_alt->columns[j].name,
                                      "=\"",
                                      safe_prometheus_key(pgexporter_get_column(j, current)),
                                      "\""
                                      );
         }

         data = pgexporter_vappend(data, 3,
                                   "} ",
                                   pgexporter_get_column_by_name(names[1], temp->query, current),
                                   "\n"
                                   );

         /* sum */
         data = pgexporter_vappend(data, 6,
                                   "pgexporter_",
                                   temp->tag,
                                   "_sum",
                                   "{server=\"",
                                   &config->servers[current->server].name[0],
                                   "\""
                                   );

         for (int j = 0; j < h_idx; j++)
         {
            data = pgexporter_vappend(data, 5,
                                      ",",
                                      temp->query_alt->columns[j].name,
                                      "=\"",
                                      safe_prometheus_key(pgexporter_get_column(j, current)),
                                      "\""
                                      );
         }

         data = pgexporter_vappend(data, 3,
                                   "} ",
                                   pgexporter_get_column_by_name(names[0], temp->query, current),
                                   "\n"
                                   );

         /* count */
         data = pgexporter_vappend(data, 6,
                                   "pgexporter_",
                                   temp->tag,
                                   "_count",
                                   "{server=\"",
                                   &config->servers[current->server].name[0],
                                   "\""
                                   );

         for (int j = 0; j < h_idx; j++)
         {
            data = pgexporter_vappend(data, 5,
                                      ",",
                                      temp->query_alt->columns[j].name,
                                      "=\"",
                                      safe_prometheus_key(pgexporter_get_column(j, current)),
                                      "\""
                                      );
         }

         data = pgexporter_vappend(data, 3,
                                   "} ",
                                   pgexporter_get_column_by_name(names[1], temp->query, current),
                                   "\n"
                                   );

         add_column_to_store(store, idx, data, temp->sort_type, current);

         current = current->next;
      }
   }
   else
   {

      /* New Column */
      if (!temp->query->tuples)
      {
         /* Skip */
         return;
      }

      (*n_store)++;

      store[idx].type = HISTOGRAM_TYPE;
      store[idx].sort_type = temp->sort_type;
      memcpy(store[idx].tag, temp->tag, MISC_LENGTH);
      memcpy(store[idx].name, temp->query_alt->columns[h_idx].name, MISC_LENGTH);

      data = NULL;
      append_help_info(&data, store[idx].tag, "", temp->query_alt->columns[h_idx].description);
      append_type_info(&data, store[idx].tag, "", temp->query_alt->columns[h_idx].type);

      add_column_to_store(store, idx, data, SORT_NAME, NULL);

      data = NULL;

      // Inserted help and type info above, and then go to append label to insert the rest of the information as usual.
      // (*n_store)++ ensures this time it fulfills the condition for the if-statement.
      goto append;
   }

}

static void
handle_gauge_counter(column_store_t* store, int* n_store, query_list_t* temp)
{
   char* data = NULL;
   struct configuration* config;
   config = (struct configuration*)shmem;

   for (int i = 0; i < temp->query_alt->n_columns; i++)
   {
      if (temp->query_alt->columns[i].type == LABEL_TYPE)
      {
         /* Dealt with later */
         continue;
      }

      int idx = 0;
      for (; idx < (*n_store); idx++)
      {
         if (!strcmp(store[idx].tag, temp->tag) &&
             ((strlen(store[idx].name) == 0 && strlen(temp->query_alt->columns[i].name) == 0) ||
              !strcmp(store[idx].name, temp->query_alt->columns[i].name)) &&
             store[idx].type == temp->query_alt->columns[i].type
             )
         {
            break;
         }
      }

append:
      if (idx < (*n_store))
      {
         /* Found Match */

         struct tuple* tuple = temp->query->tuples;

         while (tuple)
         {
            data = NULL;

            data = pgexporter_vappend(data, 2,
                                      "pgexporter_",
                                      store[idx].tag
                                      );

            if (strlen(store[idx].name) > 0)
            {
               data = pgexporter_vappend(data, 2,
                                         "_",
                                         store[idx].name
                                         );

            }

            data = pgexporter_vappend(data, 3,
                                      "{server=\"",
                                      config->servers[temp->query->tuples->server].name,
                                      "\""
                                      );

            /* Labels */
            for (int j = 0; j < temp->query_alt->n_columns; j++)
            {
               if (temp->query_alt->columns[j].type != LABEL_TYPE)
               {
                  continue;
               }

               data = pgexporter_vappend(data, 5,
                                         ",",
                                         temp->query_alt->columns[j].name,
                                         "=\"",
                                         safe_prometheus_key(pgexporter_get_column(j, tuple)),
                                         "\""
                                         );

            }

            data = pgexporter_vappend(data, 3,
                                      "} ",
                                      get_value(store[idx].tag, store[idx].name, pgexporter_get_column(i, tuple)),
                                      "\n"
                                      );

            add_column_to_store(store, idx, data, temp->sort_type, tuple);

            tuple = tuple->next;
         }

      }
      else
      {
         /* New Column */
         if (!temp->query->tuples)
         {
            /* Skip */
            continue;
         }

         (*n_store)++;

         memcpy(store[idx].name, temp->query_alt->columns[i].name, MISC_LENGTH);
         store[idx].type = temp->query_alt->columns[i].type;
         memcpy(store[idx].tag, temp->tag, MISC_LENGTH);

         data = NULL;
         append_help_info(&data, store[idx].tag, store[idx].name, temp->query_alt->columns[i].description);
         append_type_info(&data, store[idx].tag, store[idx].name, temp->query_alt->columns[i].type);

         add_column_to_store(store, idx, data, SORT_NAME, NULL);

         data = NULL;

         // Inserted help and type info above, and then go to append label to insert the rest of the information as usual.
         // (*n_store)++ ensures this time it fulfills the condition for the if-statement.
         goto append;
      }
   }
}

static void
append_help_info(char** data, char* tag, char* name, char* description)
{
   *data = pgexporter_vappend(*data, 2,
                              "#HELP pgexporter_",
                              tag
                              );

   if (strlen(name) > 0)
   {
      *data = pgexporter_vappend(*data, 2,
                                 "_",
                                 name
                                 );
   }

   *data = pgexporter_append(*data, " ");

   if (description != NULL && strcmp("", description))
   {
      *data = pgexporter_append(*data, description);
   }
   else
   {
      *data = pgexporter_vappend(*data, 2,
                                 "pgexporter_",
                                 tag
                                 );

      if (strlen(name) > 0)
      {
         *data = pgexporter_vappend(*data, 2,
                                    "_",
                                    name
                                    );
      }
   }

   *data = pgexporter_append(*data, "\n");
}

static void
append_type_info(char** data, char* tag, char* name, int typeId)
{
   *data = pgexporter_vappend(*data, 2,
                              "#TYPE pgexporter_",
                              tag
                              );

   if (strlen(name) > 0)
   {
      *data = pgexporter_vappend(*data, 2,
                                 "_",
                                 name
                                 );
   }

   if (typeId == GAUGE_TYPE)
   {
      *data = pgexporter_append(*data, " gauge");
   }
   else if (typeId == COUNTER_TYPE)
   {
      *data = pgexporter_append(*data, " counter");
   }
   else if (typeId == HISTOGRAM_TYPE)
   {
      *data = pgexporter_append(*data, " histogram");
   }

   *data = pgexporter_append(*data, "\n");
}

static int
send_chunk(int client_fd, char* data)
{
   int status;
   char* m = NULL;
   struct message msg;

   memset(&msg, 0, sizeof(struct message));

   m = malloc(20);
   memset(m, 0, 20);

   sprintf(m, "%lX\r\n", strlen(data));

   m = pgexporter_vappend(m, 2,
                          data,
                          "\r\n"
                          );

   msg.kind = 0;
   msg.length = strlen(m);
   msg.data = m;

   status = pgexporter_write_message(NULL, client_fd, &msg);

   free(m);

   return status;
}

static char*
get_value(char* tag, char* name, char* val)
{
   char* end = NULL;

   /* Empty to 0 */
   if (val == NULL || !strcmp(val, ""))
   {
      return "0";
   }

   /* Bool */
   if (!strcmp(val, "off") || !strcmp(val, "f") || !strcmp(val, "(disabled)"))
   {
      return "0";
   }
   else if (!strcmp(val, "on") || !strcmp(val, "t"))
   {
      return "1";
   }

   if (!strcmp(val, "NaN"))
   {
      return val;
   }

   /* long */
   strtol(val, &end, 10);
   if (*end == '\0')
   {
      return val;
   }
   errno = 0;

   /* double */
   strtod(val, &end);
   if (*end == '\0')
   {
      return val;
   }
   errno = 0;

   pgexporter_log_trace("get_value(%s/%s): %s", tag, name, val);

   /* Map general strings to 1 */
   return "1";
}

static char*
safe_prometheus_key(char* key)
{
   int i = 0;

   if (key == NULL)
   {
      return "";
   }

   while (key[i] != '\0')
   {
      if (key[i] == '.')
      {
         if (i == strlen(key) - 1)
         {
            key[i] = '\0';
         }
         else
         {
            key[i] = '_';
         }
      }
      i++;
   }
   return key;
}

/**
 * Checks if the Prometheus cache configuration setting
 * (`metrics_cache`) has a non-zero value, that means there
 * are seconds to cache the response.
 *
 * @return true if there is a cache configuration,
 *         false if no cache is active
 */
static bool
is_metrics_cache_configured(void)
{
   struct configuration* config;

   config = (struct configuration*)shmem;

   // cannot have caching if not set metrics!
   if (config->metrics == 0)
   {
      return false;
   }

   return config->metrics_cache_max_age != PGEXPORTER_PROMETHEUS_CACHE_DISABLED;
}

/**
 * Checks if the cache is still valid, and therefore can be
 * used to serve as a response.
 * A cache is considred valid if it has non-empty payload and
 * a timestamp in the future.
 *
 * @return true if the cache is still valid
 */
static bool
is_metrics_cache_valid(void)
{
   time_t now;

   struct prometheus_cache* cache;

   cache = (struct prometheus_cache*)prometheus_cache_shmem;

   if (cache->valid_until == 0 || strlen(cache->data) == 0)
   {
      return false;
   }

   now = time(NULL);
   return now <= cache->valid_until;
}

int
pgexporter_init_prometheus_cache(size_t* p_size, void** p_shmem)
{
   struct prometheus_cache* cache;
   struct configuration* config;
   size_t cache_size = 0;
   size_t struct_size = 0;

   config = (struct configuration*)shmem;

   // first of all, allocate the overall cache structure
   cache_size = metrics_cache_size_to_alloc();
   struct_size = sizeof(struct prometheus_cache);

   if (pgexporter_create_shared_memory(struct_size + cache_size, config->hugepage, (void*) &cache))
   {
      goto error;
   }

   memset(cache, 0, struct_size + cache_size);
   cache->valid_until = 0;
   cache->size = cache_size;
   atomic_init(&cache->lock, STATE_FREE);

   // success! do the memory swap
   *p_shmem = cache;
   *p_size = cache_size + struct_size;
   return 0;

error:
   // disable caching
   config->metrics_cache_max_age = config->metrics_cache_max_size = PGEXPORTER_PROMETHEUS_CACHE_DISABLED;
   pgexporter_log_error("Cannot allocate shared memory for the Prometheus cache!");
   *p_size = 0;
   *p_shmem = NULL;

   return 1;
}

/**
 * Provides the size of the cache to allocate.
 *
 * It checks if the metrics cache is configured, and
 * computers the right minimum value between the
 * user configured requested size and the default
 * cache size.
 *
 * @return the cache size to allocate
 */
static size_t
metrics_cache_size_to_alloc(void)
{
   struct configuration* config;
   size_t cache_size = 0;

   config = (struct configuration*)shmem;

   // which size to use ?
   // either the configured (i.e., requested by user) if lower than the max size
   // or the default value
   if (is_metrics_cache_configured())
   {
      cache_size = config->metrics_cache_max_size > 0
            ? MIN(config->metrics_cache_max_size, PROMETHEUS_MAX_CACHE_SIZE)
            : PROMETHEUS_DEFAULT_CACHE_SIZE;
   }

   return cache_size;
}

/**
 * Invalidates the cache.
 *
 * Requires the caller to hold the lock on the cache!
 *
 * Invalidating the cache means that the payload is zero-filled
 * and that the valid_until field is set to zero too.
 */
static void
metrics_cache_invalidate(void)
{
   struct prometheus_cache* cache;

   cache = (struct prometheus_cache*)prometheus_cache_shmem;

   memset(cache->data, 0, cache->size);
   cache->valid_until = 0;
}

/**
 * Appends data to the cache.
 *
 * Requires the caller to hold the lock on the cache!
 *
 * If the input data is empty, nothing happens.
 * The data is appended only if the cache does not overflows, that
 * means the current size of the cache plus the size of the data
 * to append does not exceed the current cache size.
 * If the cache overflows, the cache is flushed and marked
 * as invalid.
 * This makes safe to call this method along the workflow of
 * building the Prometheus response.
 *
 * @param data the string to append to the cache
 * @return true on success
 */
static bool
metrics_cache_append(char* data)
{
   int origin_length = 0;
   int append_length = 0;
   struct prometheus_cache* cache;

   cache = (struct prometheus_cache*)prometheus_cache_shmem;

   if (!is_metrics_cache_configured())
   {
      return false;
   }

   origin_length = strlen(cache->data);
   append_length = strlen(data);
   // need to append the data to the cache
   if (origin_length + append_length >= cache->size)
   {
      // cannot append new data, so invalidate cache
      pgexporter_log_debug("Cannot append %d bytes to the Prometheus cache because it will overflow the size of %d bytes (currently at %d bytes). HINT: try adjusting `metrics_cache_max_size`",
                           append_length,
                           cache->size,
                           origin_length);
      metrics_cache_invalidate();
      return false;
   }

   // append the data to the data field
   memcpy(cache->data + origin_length, data, append_length);
   cache->data[origin_length + append_length + 1] = '\0';
   return true;
}

/**
 * Finalizes the cache.
 *
 * Requires the caller to hold the lock on the cache!
 *
 * This method should be invoked when the cache is complete
 * and therefore can be served.
 *
 * @return true if the cache has a validity
 */
static bool
metrics_cache_finalize(void)
{
   struct configuration* config;
   struct prometheus_cache* cache;
   time_t now;

   cache = (struct prometheus_cache*)prometheus_cache_shmem;
   config = (struct configuration*)shmem;

   if (!is_metrics_cache_configured())
   {
      return false;
   }

   now = time(NULL);
   cache->valid_until = now + config->metrics_cache_max_age;
   return cache->valid_until > now;
}
