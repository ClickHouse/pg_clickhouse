#include <iostream>
#include <clickhouse/client.h>
#include "include/binary.hh"
#include "include/internal.h"
#include "postgres.h"
#include "pgtime.h"
#include "funcapi.h"
#include "fmgr.h"

using namespace clickhouse;

extern "C" {
    ch_binary_connection_t * ch_binary_connect(
        char * host, int port, char * database, char * user, char * password, char ** error)
    {
        ClientOptions * options = NULL;
        ch_binary_connection_t * conn = NULL;

        try
        {
            options = new ClientOptions();
            options->SetPingBeforeQuery(true);

            if (host)
                options->SetHost(std::string(host));
            if (port)
                options->SetPort(port);
            if (database)
                options->SetDefaultDatabase(std::string(database));
            if (user)
                options->SetUser(std::string(user));
            if (password)
                options->SetPassword(std::string(password));

            //options->SetRethrowException(false);
            conn = new ch_binary_connection_t();

            Client * client = new Client(*options);
            conn->client = client;
            conn->options = options;
        }
        catch (const std::exception & e)
        {
            if (error)
                *error = strdup(e.what());

            if (conn != NULL)
                delete conn;

            if (options != NULL)
                delete options;

            conn = NULL;
        }
        return conn;
        }
}