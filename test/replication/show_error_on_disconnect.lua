#!/usr/bin/env tarantool

-- get instance name from filename (show_error_on_disconnect1.lua => show_error_on_disconnect)
local INSTANCE_ID = string.match(arg[0], "%d")

local SOCKET_DIR = require('fio').cwd()

local TIMEOUT = tonumber(arg[1])
local CON_TIMEOUT = arg[2] and tonumber(arg[2]) or 60.0

local function instance_uri(instance_id)
    --return 'localhost:'..(3310 + instance_id)
    return SOCKET_DIR..'/show_error_on_disconnect'..instance_id..'.sock';
end

-- start console first
require('console').listen(os.getenv('ADMIN'))

box.cfg({
    listen = instance_uri(INSTANCE_ID);
--    log_level = 7;
    replication = {
        instance_uri(1);
        instance_uri(2);
    };
    replication_connect_quorum = 0;
    replication_timeout = TIMEOUT;
    replication_connect_timeout = CON_TIMEOUT;
})

test_run = require('test_run').new()
engine = test_run:get_cfg('engine')

box.once("bootstrap", function()
    box.schema.user.grant("guest", 'replication')
    box.schema.space.create('test', {engine = engine})
    box.space.test:create_index('primary')
end)