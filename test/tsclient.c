/*
 * Copyright (C) 2025 The pgexporter community
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
 *
 */

/* pgexporter */
#include <pgexporter.h>
#include <configuration.h>
#include <json.h>
#include <logging.h>
#include <management.h>
#include <memory.h>
#include <network.h>
#include <queries.h>
#include <security.h>
#include <shmem.h>
#include <utils.h>
#include <value.h>
#include <tsclient.h>

/* system */
#include <err.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/types.h>

char project_directory[BUFFER_SIZE];

static int check_output_outcome(int socket);
static int get_connection();
static char* get_configuration_path();

int
pgexporter_tsclient_init(char* base_dir)
{
    struct configuration* config = NULL;
    int ret;
    size_t size;
    char* configuration_path = NULL;

    memset(project_directory, 0, sizeof(project_directory));
    memcpy(project_directory, base_dir, strlen(base_dir));

    configuration_path = get_configuration_path();

    // Initialize memory subsystem first
    pgexporter_memory_init();

    // Create the shared memory for the configuration
    size = sizeof(struct configuration);
    if (pgexporter_create_shared_memory(size, HUGEPAGE_OFF, &shmem))
    {
        goto error;
    }
    
    pgexporter_init_configuration(shmem);
    
    // Try reading configuration from the configuration path
    if (configuration_path != NULL)
    {
        ret = pgexporter_read_configuration(shmem, configuration_path);
        if (ret)
        {
            goto error;
        }

        config = (struct configuration*)shmem;
        
        // Initialize logging subsystem
        if (pgexporter_init_logging())
        {
            goto error;
        }

        if (pgexporter_start_logging())
        {
            goto error;
        }
    }
    else
    {
        goto error;
    } 

    free(configuration_path);
    return 0;
    
error:
    free(configuration_path);
    return 1;
}

int
pgexporter_tsclient_destroy()
{
    size_t size;

    // Stop logging
    pgexporter_stop_logging();
    
    // Destroy shared memory
    size = sizeof(struct configuration);
    pgexporter_destroy_shared_memory(shmem, size);
    
    // Destroy memory subsystem
    pgexporter_memory_destroy();
    
    return 0;
}

int
pgexporter_tsclient_execute_ping()
{
    int socket = -1;
    
    socket = get_connection();
    
    // Security Checks
    if (!pgexporter_socket_isvalid(socket))
    {
        goto error;
    }
    
    // Create a ping request to the main server
    if (pgexporter_management_request_ping(NULL, socket, MANAGEMENT_COMPRESSION_NONE, MANAGEMENT_ENCRYPTION_NONE, MANAGEMENT_OUTPUT_FORMAT_JSON))
    {
        goto error;
    }

    // Check the outcome field of the output, if true success, else failure
    if (check_output_outcome(socket))
    {
        goto error;
    }

    pgexporter_disconnect(socket);
    return 0;
    
error:
    pgexporter_disconnect(socket);
    return 1;
}

int
pgexporter_tsclient_execute_shutdown()
{
    int socket = -1;
    
    socket = get_connection();
    
    // Security Checks
    if (!pgexporter_socket_isvalid(socket))
    {
        goto error;
    }
    
    // Create a shutdown request to the main server
    if (pgexporter_management_request_shutdown(NULL, socket, MANAGEMENT_COMPRESSION_NONE, MANAGEMENT_ENCRYPTION_NONE, MANAGEMENT_OUTPUT_FORMAT_JSON))
    {
        goto error;
    }

    // Check the outcome field of the output, if true success, else failure
    if (check_output_outcome(socket))
    {
        goto error;
    }

    pgexporter_disconnect(socket);
    return 0;
    
error:
    pgexporter_disconnect(socket);
    return 1;
}

int
pgexporter_tsclient_execute_status()
{
    int socket = -1;
    
    socket = get_connection();
    
    // Security Checks
    if (!pgexporter_socket_isvalid(socket))
    {
        goto error;
    }
    
    // Create a status request to the main server
    if (pgexporter_management_request_status(NULL, socket, MANAGEMENT_COMPRESSION_NONE, MANAGEMENT_ENCRYPTION_NONE, MANAGEMENT_OUTPUT_FORMAT_JSON))
    {
        goto error;
    }

    // Check the outcome field of the output, if true success, else failure
    if (check_output_outcome(socket))
    {
        goto error;
    }

    pgexporter_disconnect(socket);
    return 0;
    
error:
    pgexporter_disconnect(socket);
    return 1;
}

int
pgexporter_tsclient_test_db_connection()
{
    struct configuration* config;
    int connected_servers = 0;

    config = (struct configuration*)shmem;

    printf("Testing database connections...\n");
    
    // Validate configuration first
    if (pgexporter_validate_configuration(shmem))
    {
        printf("Configuration validation failed\n");
        return 1;
    }
    
    if (pgexporter_validate_users_configuration(shmem))
    {
        printf("Users configuration validation failed\n");
        return 1;
    }
    
    printf("Number of configured servers: %d\n", config->number_of_servers);
    
    // Test opening connections
    pgexporter_open_connections();

    // Check how many servers are connected
    for (int i = 0; i < config->number_of_servers; i++)
    {
        printf("Server %s: ", config->servers[i].name);
        if (config->servers[i].fd != -1)
        {
            printf("Connected (fd=%d)\n", config->servers[i].fd);
            connected_servers++;
        }
        else
        {
            printf("Not connected\n");
        }
    }

    printf("Total connected servers: %d/%d\n", connected_servers, config->number_of_servers);

    // Clean up connections
    pgexporter_close_connections();

    // Return success if at least one server connected
    return (connected_servers > 0) ? 0 : 1;
}

int
pgexporter_tsclient_test_version_query()
{
    struct configuration* config;
    struct query* query = NULL;
    struct tuple* current = NULL;
    int ret = 1;
    int server_tested = 0;

    config = (struct configuration*)shmem;

    printf("Testing PostgreSQL version query...\n");
    
    // Validate configuration first
    if (pgexporter_validate_configuration(shmem))
    {
        printf("Configuration validation failed\n");
        return 1;
    }
    
    if (pgexporter_validate_users_configuration(shmem))
    {
        printf("Users configuration validation failed\n");
        return 1;
    }
    
    // Open connections first
    pgexporter_open_connections();

    // Test version query on first available server
    for (int i = 0; i < config->number_of_servers && !server_tested; i++)
    {
        if (config->servers[i].fd != -1)
        {
            printf("Testing version query on server %s...\n", config->servers[i].name);
            
            if (pgexporter_query_version(i, &query) == 0 && query != NULL)
            {
                current = query->tuples;
                if (current != NULL)
                {
                    printf("PostgreSQL Version: %s.%s\n", 
                           pgexporter_get_column(0, current),
                           pgexporter_get_column(1, current));
                    ret = 0;
                    server_tested = 1;
                }
                else
                {
                    printf("No version data returned\n");
                }
                pgexporter_free_query(query);
            }
            else
            {
                printf("Failed to execute version query\n");
            }
        }
    }

    if (!server_tested)
    {
        printf("No servers available for version query test\n");
    }

    // Clean up connections
    pgexporter_close_connections();

    return ret;
}

int
pgexporter_tsclient_test_extension_path()
{
    struct configuration* config;
    char* bin_path = NULL;
    int ret = 1;

    config = (struct configuration*)shmem;

    printf("Testing extension path setup...\n");
    
    // Validate configuration first
    if (pgexporter_validate_configuration(shmem))
    {
        printf("Configuration validation failed\n");
        return 1;
    }
    
    // Use a real program path from the project directory
    char* program_path = NULL;
    program_path = pgexporter_append(program_path, project_directory);
    program_path = pgexporter_append(program_path, "/src/pgexporter");
    
    printf("Using program path: %s\n", program_path);
    
    // Test extension path setup
    if (pgexporter_setup_extensions_path(config, program_path, &bin_path) == 0)
    {
        if (bin_path != NULL && strlen(bin_path) > 0)
        {
            printf("Extension path setup successful: %s\n", bin_path);
            ret = 0;
        }
        else
        {
            printf("Extension path setup returned success but path is empty or null\n");
        }
    }
    else
    {
        printf("Extension path setup failed\n");
    }

    // Print the path regardless of success/failure for debugging
    if (bin_path != NULL)
    {
        printf("Final extension path: %s\n", bin_path);
        free(bin_path);
    }
    else
    {
        printf("Extension path is NULL\n");
    }
    
    // Print the configured extensions path from config
    printf("Configured extensions path: %s\n", config->extensions_path);

    free(program_path);
    return ret;
}

static int 
check_output_outcome(int socket)
{
    struct json* read = NULL;
    struct json* outcome = NULL;

    if (pgexporter_management_read_json(NULL, socket, NULL, NULL, &read))
    {
        goto error;
    }
    
    if (!pgexporter_json_contains_key(read, MANAGEMENT_CATEGORY_OUTCOME))
    {
        goto error;
    }

    outcome = (struct json*)pgexporter_json_get(read, MANAGEMENT_CATEGORY_OUTCOME);
    if (!pgexporter_json_contains_key(outcome, MANAGEMENT_ARGUMENT_STATUS) || !(bool)pgexporter_json_get(outcome, MANAGEMENT_ARGUMENT_STATUS))
    {
        goto error;
    }

    pgexporter_json_destroy(read);
    return 0;
    
error:
    pgexporter_json_destroy(read);
    return 1;
}

static int
get_connection()
{
    int socket = -1;
    struct configuration* config;

    config = (struct configuration*)shmem;
    
    if (pgexporter_connect_unix_socket(config->unix_socket_dir, MAIN_UDS, &socket))
    {
        return -1;
    }
    
    return socket;
}

static char*
get_configuration_path()
{
   char* configuration_path = NULL;
   int project_directory_length = strlen(project_directory);
   int configuration_trail_length = strlen(PGEXPORTER_CONFIGURATION_TRAIL);

   configuration_path = (char*)calloc(project_directory_length + configuration_trail_length + 1, sizeof(char));

   memcpy(configuration_path, project_directory, project_directory_length);
   memcpy(configuration_path + project_directory_length, PGEXPORTER_CONFIGURATION_TRAIL, configuration_trail_length);

   return configuration_path;
}