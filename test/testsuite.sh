#!/bin/bash
#
# Copyright (C) 2025 The pgexporter community
#
# Basic test suite for pgexporter, more to be added later

set -e

OS=$(uname)
THIS_FILE=$(realpath "$0")
USER=$(whoami)
WAIT_TIMEOUT=10

PORT=5432
METRICS_PORT=5002
PGPASSWORD="password"

PROJECT_DIRECTORY=$(pwd)
EXECUTABLE_DIRECTORY=$(pwd)/src
TEST_DIRECTORY=$(pwd)/test

LOG_DIRECTORY=$(pwd)/log
PGCTL_LOG_FILE=$LOG_DIRECTORY/logfile
PGEXPORTER_LOG_FILE=$LOG_DIRECTORY/pgexporter.log

POSTGRES_OPERATION_DIR=$(pwd)/pgexporter-postgresql
DATA_DIRECTORY=$POSTGRES_OPERATION_DIR/data

PGEXPORTER_OPERATION_DIR=$(pwd)/pgexporter-testsuite
CONFIGURATION_DIRECTORY=$PGEXPORTER_OPERATION_DIR/conf

PSQL_USER=$USER
if [ "$OS" = "FreeBSD" ]; then
  PSQL_USER=postgres
fi

########################### UTILS ############################
is_port_in_use() {
   local port=$1
   if [[ "$OS" == "Linux" ]]; then
      ss -tuln | grep $port >/dev/null 2>&1
   elif [[ "$OS" == "Darwin" ]]; then
      lsof -i:$port >/dev/null 2>&1
   elif [[ "$OS" == "FreeBSD" ]]; then
      sockstat -4 -l | grep $port >/dev/null 2>&1
   fi
   return $?
}

next_available_port() {
   local port=$1
   while true; do
      is_port_in_use $port
      if [ $? -ne 0 ]; then
         echo "$port"
         return 0
      else
         port=$((port + 1))
      fi
   done
}

wait_for_server_ready() {
   local start_time=$SECONDS
   while true; do
      pg_isready -h localhost -p $PORT
      if [ $? -eq 0 ]; then
         echo "PostgreSQL is ready for accepting responses"
         return 0
      fi
      if [ $(($SECONDS - $start_time)) -gt $WAIT_TIMEOUT ]; then
         echo "waiting for PostgreSQL timed out"
         return 1
      fi
      sleep 1
   done
}

wait_for_pgexporter_ready() {
   local start_time=$SECONDS
   while true; do
      curl -s http://localhost:$METRICS_PORT/metrics >/dev/null 2>&1
      if [ $? -eq 0 ]; then
         echo "pgexporter is ready for accepting responses"
         return 0
      fi
      if [ $(($SECONDS - $start_time)) -gt $WAIT_TIMEOUT ]; then
         echo "waiting for pgexporter timed out"
         return 1
      fi
      sleep 1
   done
}

function sed_i() {
   if [[ "$OS" == "Darwin" || "$OS" == "FreeBSD" ]]; then
      sed -i '' -E "$@"
   else
      sed -i -E "$@"
   fi
}

##############################################################

check_system_requirements() {
   echo -e "\e[34mCheck System Requirements \e[0m"
   echo "check system os ... $OS"
   
   if ! which initdb >/dev/null 2>&1; then
      echo "check initdb in path ... not present"
      exit 1
   fi
   echo "check initdb in path ... ok"
   
   if ! which pg_ctl >/dev/null 2>&1; then
      echo "check pg_ctl in path ... not ok"
      exit 1
   fi
   echo "check pg_ctl in path ... ok"
   
   if ! which psql >/dev/null 2>&1; then
      echo "check psql in path ... not present"
      exit 1
   fi
   echo "check psql in path ... ok"
   
   echo ""
}

initialize_log_files() {
   echo -e "\e[34mInitialize Test logfiles \e[0m"
   mkdir -p $LOG_DIRECTORY
   echo "create log directory ... $LOG_DIRECTORY"
   touch $PGEXPORTER_LOG_FILE
   echo "create log file ... $PGEXPORTER_LOG_FILE"
   touch $PGCTL_LOG_FILE
   echo "create log file ... $PGCTL_LOG_FILE"
   echo ""
}

create_cluster() {
   echo -e "\e[34mInitializing PostgreSQL Cluster \e[0m"
   mkdir -p "$POSTGRES_OPERATION_DIR"
   mkdir -p "$DATA_DIRECTORY"
   mkdir -p $CONFIGURATION_DIRECTORY

   if [ "$OS" = "FreeBSD" ]; then
    if ! pw user show postgres >/dev/null 2>&1; then
        pw groupadd -n postgres -g 770
        pw useradd -n postgres -u 770 -g postgres -d /var/db/postgres -s /bin/sh
    fi
    chown postgres:postgres $PGCTL_LOG_FILE
    chown -R postgres:postgres "$DATA_DIRECTORY"
    chown -R postgres:postgres $CONFIGURATION_DIRECTORY
   fi

   INITDB_PATH=$(command -v initdb)
   if [ "$OS" = "FreeBSD" ]; then
     su - postgres -c "$INITDB_PATH -k -D $DATA_DIRECTORY"
   else
     "$INITDB_PATH" -k -D $DATA_DIRECTORY
   fi
   
   echo "initialize database ... ok"
   
   # Configure PostgreSQL
   sed_i "s/^#[[:space:]]*password_encryption[[:space:]]*=[[:space:]]*(md5|scram-sha-256)/password_encryption = scram-sha-256/" "$DATA_DIRECTORY/postgresql.conf"
   sed_i "s|#unix_socket_directories = '/var/run/postgresql'|unix_socket_directories = '/tmp'|" $DATA_DIRECTORY/postgresql.conf
   sed_i "s/#port = 5432/port = $PORT/" $DATA_DIRECTORY/postgresql.conf
   sed_i "s/#wal_level = replica/wal_level = replica/" $DATA_DIRECTORY/postgresql.conf
   
   echo ""
}

initialize_hba_configuration() {
   echo -e "\e[34mCreate HBA Configuration \e[0m"
   echo "
    local   all              all                                     trust
    local   replication      all                                     trust
    host    postgres         pgexporter      127.0.0.1/32            scram-sha-256
    host    postgres         pgexporter      ::1/128                 scram-sha-256
    " >$DATA_DIRECTORY/pg_hba.conf
   echo "initialize hba configuration at $DATA_DIRECTORY/pg_hba.conf ... ok"
   echo ""
}

start_postgresql() {
   echo -e "\e[34mStarting PostgreSQL \e[0m"
   PGCTL_PATH=$(command -v pg_ctl)
   
   if [ "$OS" = "FreeBSD" ]; then
     su - postgres -c "$PGCTL_PATH -D $DATA_DIRECTORY -l $PGCTL_LOG_FILE start"
   else
     "$PGCTL_PATH" -D $DATA_DIRECTORY -l $PGCTL_LOG_FILE start
   fi
   
   wait_for_server_ready
   if [ $? -ne 0 ]; then
      echo "PostgreSQL failed to start"
      exit 1
   fi
   
   # Create pgexporter user with monitoring privileges
   psql -h /tmp -p $PORT -U $PSQL_USER -d postgres -c "CREATE USER pgexporter WITH PASSWORD '$PGPASSWORD';" || true
   psql -h /tmp -p $PORT -U $PSQL_USER -d postgres -c "GRANT pg_monitor TO pgexporter;" || true
   
   echo "PostgreSQL setup complete"
   echo ""
}

stop_postgresql() {
   PGCTL_PATH=$(command -v pg_ctl)
   if [ "$OS" = "FreeBSD" ]; then
     su - postgres -c "$PGCTL_PATH -D $DATA_DIRECTORY -l $PGCTL_LOG_FILE stop"
   else
     "$PGCTL_PATH" -D $DATA_DIRECTORY -l $PGCTL_LOG_FILE stop
   fi
}

initialize_pgexporter_configuration() {
   echo -e "\e[34mInitialize pgexporter configuration \e[0m"
   
   cat <<EOF >$CONFIGURATION_DIRECTORY/pgexporter.conf
[pgexporter]
host = localhost
metrics = $METRICS_PORT
log_type = file
log_level = debug5
log_path = $PGEXPORTER_LOG_FILE
unix_socket_dir = /tmp/

[primary]
host = localhost
port = $PORT
user = pgexporter
EOF

   cat <<EOF >$CONFIGURATION_DIRECTORY/pgexporter_users.conf
pgexporter:$PGPASSWORD
EOF

   echo "pgexporter configuration created ... ok"
   echo ""
}

start_pgexporter() {
   echo -e "\e[34mStarting pgexporter \e[0m"
   
   # Start pgexporter in daemon mode
   $EXECUTABLE_DIRECTORY/pgexporter -c $CONFIGURATION_DIRECTORY/pgexporter.conf -u $CONFIGURATION_DIRECTORY/pgexporter_users.conf -d
   
   wait_for_pgexporter_ready
   if [ $? -ne 0 ]; then
      echo "pgexporter failed to start"
      exit 1
   fi
   
   echo "pgexporter started successfully"
   echo ""
}

stop_pgexporter() {
   echo "Stopping pgexporter..."
   pkill -f pgexporter || true
   sleep 2
}

execute_tests() {
   echo -e "\e[34mExecute Test Cases \e[0m"
   
   # Run the actual test executable
   if [ -f "$TEST_DIRECTORY/pgexporter_test" ]; then
      $TEST_DIRECTORY/pgexporter_test
   else
      echo "Test executable not found. Building tests..."
      cd build
      make pgexporter_test
      ./test/pgexporter_test
   fi
   
   echo "Tests completed"
   echo ""
}

clean() {
   echo -e "\e[34mClean Test Resources \e[0m"
   
   stop_pgexporter
   stop_postgresql
   
   if [ -d $POSTGRES_OPERATION_DIR ]; then
      rm -rf $POSTGRES_OPERATION_DIR
      echo "remove postgres operations directory ... ok"
   fi

   if [ -d $PGEXPORTER_OPERATION_DIR ]; then
      rm -rf $PGEXPORTER_OPERATION_DIR
      echo "remove pgexporter operations directory ... ok"
   fi
   
   if [ -d $LOG_DIRECTORY ]; then
      rm -rf $LOG_DIRECTORY
      echo "remove log directory ... ok"
   fi
}

run_tests() {
   check_system_requirements
   initialize_log_files
   
   PORT=$(next_available_port $PORT)
   METRICS_PORT=$(next_available_port $METRICS_PORT)
   
   create_cluster
   initialize_hba_configuration
   start_postgresql
   initialize_pgexporter_configuration
   start_pgexporter
   execute_tests
   clean
}

usage() {
   echo "Usage: $0 [clean]"
   echo "  clean    - clean up test environment"
   exit 1
}

if [ $# -gt 1 ]; then
   usage
elif [ $# -eq 1 ]; then
   if [ "$1" == "clean" ]; then
      clean
   else
      echo "Invalid parameter: $1"
      usage
   fi
else
   run_tests
fi