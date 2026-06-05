#sudo apt-get install libmariadb-dev-compat
gcc iotdb_select.c -o iotdb_select -lmysqlclient
gcc iotdb_insert.c -o iotdb_insert -lmysqlclient
gcc iotdb_update.c -o iotdb_update -lmysqlclient
