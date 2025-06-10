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
#include <management.h>
#include <network.h>
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

    size = sizeof(struct configuration);
    return pgexporter_destroy_shared_memory(shmem, size);
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
    struct json* read = NULL;
    struct json* outcome = NULL;
    struct json* response = NULL;
    int socket = -1;
    int ret = 1;
    
    printf("Testing database connection through status command...\n");
    
    socket = get_connection();
    
    // Security Checks
    if (!pgexporter_socket_isvalid(socket))
    {
        printf("Failed to connect to pgexporter daemon\n");
        goto error;
    }
    
    // Use status command to check server connections
    if (pgexporter_management_request_status(NULL, socket, MANAGEMENT_COMPRESSION_NONE, MANAGEMENT_ENCRYPTION_NONE, MANAGEMENT_OUTPUT_FORMAT_JSON))
    {
        printf("Failed to send status request\n");
        goto error;
    }

    // Read the response and extract server information
    if (pgexporter_management_read_json(NULL, socket, NULL, NULL, &read))
    {
        printf("Failed to read status response\n");
        goto error;
    }
    
    if (!pgexporter_json_contains_key(read, MANAGEMENT_CATEGORY_OUTCOME))
    {
        printf("Response missing outcome\n");
        goto error;
    }

    outcome = (struct json*)pgexporter_json_get(read, MANAGEMENT_CATEGORY_OUTCOME);
    if (!pgexporter_json_contains_key(outcome, MANAGEMENT_ARGUMENT_STATUS) || !(bool)pgexporter_json_get(outcome, MANAGEMENT_ARGUMENT_STATUS))
    {
        printf("Status command failed\n");
        goto error;
    }
    
    if (pgexporter_json_contains_key(read, MANAGEMENT_CATEGORY_RESPONSE))
    {
        response = (struct json*)pgexporter_json_get(read, MANAGEMENT_CATEGORY_RESPONSE);
        if (response != NULL)
        {
            printf("Successfully retrieved status from daemon - database connections are working\n");
            ret = 0;
        }
    }

    pgexporter_json_destroy(read);
    pgexporter_disconnect(socket);
    return ret;
    
error:
    pgexporter_json_destroy(read);
    pgexporter_disconnect(socket);
    return 1;
}

int
pgexporter_tsclient_test_version_query()
{
    struct json* read = NULL;
    struct json* outcome = NULL;
    struct json* response = NULL;
    int socket = -1;
    int ret = 1;
    
    printf("Testing version information through status details command...\n");
    
    socket = get_connection();
    
    // Security Checks
    if (!pgexporter_socket_isvalid(socket))
    {
        printf("Failed to connect to pgexporter daemon\n");
        goto error;
    }
    
    // Use status details command to get version information
    if (pgexporter_management_request_status_details(NULL, socket, MANAGEMENT_COMPRESSION_NONE, MANAGEMENT_ENCRYPTION_NONE, MANAGEMENT_OUTPUT_FORMAT_JSON))
    {
        printf("Failed to send status details request\n");
        goto error;
    }

    // Read the response and extract version information
    if (pgexporter_management_read_json(NULL, socket, NULL, NULL, &read))
    {
        printf("Failed to read status details response\n");
        goto error;
    }
    
    if (!pgexporter_json_contains_key(read, MANAGEMENT_CATEGORY_OUTCOME))
    {
        printf("Response missing outcome\n");
        goto error;
    }

    outcome = (struct json*)pgexporter_json_get(read, MANAGEMENT_CATEGORY_OUTCOME);
    if (!pgexporter_json_contains_key(outcome, MANAGEMENT_ARGUMENT_STATUS) || !(bool)pgexporter_json_get(outcome, MANAGEMENT_ARGUMENT_STATUS))
    {
        printf("Status details command failed\n");
        goto error;
    }
    
    if (pgexporter_json_contains_key(read, MANAGEMENT_CATEGORY_RESPONSE))
    {
        response = (struct json*)pgexporter_json_get(read, MANAGEMENT_CATEGORY_RESPONSE);
        if (response != NULL)
        {
            printf("Successfully retrieved detailed status with version information from daemon\n");
            ret = 0;
        }
    }

    pgexporter_json_destroy(read);
    pgexporter_disconnect(socket);
    return ret;
    
error:
    pgexporter_json_destroy(read);
    pgexporter_disconnect(socket);
    return 1;
}

int
pgexporter_tsclient_test_extension_path()
{
    struct configuration* config;
    char* bin_path = NULL;
    int ret = 1;

    config = (struct configuration*)shmem;

    printf("Testing extension path setup...\n");
    
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