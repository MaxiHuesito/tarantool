env = require('test_run')
test_run = env.new()
test_run:cmd("setopt delimiter ';'")
-- empty transaction - ok
box.begin() box.commit();
-- double begin
box.begin() box.begin();
-- no active transaction since exception rolled it back
box.commit();
-- double commit - implicit start of transaction
box.begin() box.commit() box.commit();
-- commit if not started - implicit start of transaction
box.commit();
-- rollback if not started - ok
box.rollback()
-- double rollback - ok
box.begin() box.rollback() box.rollback();
-- rollback of an empty trans - ends transaction
box.begin() box.rollback();
-- no current transaction - implicit begin
box.commit();

--
-- gh-3518: Add is_in_txn().
--
-- no active transaction - false
box.is_in_txn();
-- active transaction - true
box.begin();
box.is_in_txn();
-- no active transaction - false
box.commit();
box.is_in_txn();

fiber = require('fiber');
function sloppy()
    box.begin()
end;
f = fiber.create(sloppy);
-- when the sloppy fiber ends, its session has an active transction
-- ensure it's rolled back automatically
while f:status() ~= 'dead' do fiber.sleep(0) end;
-- transactions and system spaces
-- some operation involves more than one ddl spaces, so they should fail
box.begin() box.schema.space.create('test');
box.rollback();
box.begin() box.schema.user.create('test');
box.rollback();
-- but this is Ok now
box.begin() box.schema.func.create('test') box.rollback();
box.begin() box.schema.user.grant('guest', 'read', 'space', '_priv') box.rollback();
space = box.schema.space.create('test');
box.begin() box.space._space:delete{space.id} box.rollback();
box.begin() box.space._space:delete{space.id} box.commit();
box.begin() box.space._sequence:insert{1, 1, 'test', 1, 1, 2, 1, 0, false} box.rollback();
box.begin() box.space._schema:insert{'test'} box.rollback();
box.begin() box.space._cluster:insert{30, '00000000-0000-0000-0000-000000000001'} box.rollback();
s = box.schema.space.create('test');
box.begin() index = s:create_index('primary') box.rollback();
index = s:create_index('primary');
t = nil
function multi()
    box.begin()
    s:auto_increment{'first row'}
    s:auto_increment{'second row'}
    t = s:select{}
    box.commit()
end;
multi();
t;
s:select{};
s:truncate();
function multi()
    box.begin()
    s:auto_increment{'first row'}
    s:auto_increment{'second row'}
    t = s:select{}
    box.rollback()
end;
multi();
t;
s:select{};
function multi()
    box.begin()
    s:insert{1, 'first row'}
    pcall(s.insert, s, {1, 'duplicate'})
    t = s:select{}
    box.commit()
end;
multi();
t;
s:select{};
s:truncate();
--
-- Test that fiber yield causes a transaction rollback
-- but only if the transaction has changed any data
--
-- Test admin console
box.begin();
-- should be ok - active transaction, and we don't
-- know, maybe it will use vinyl engine, which
-- may support yield() in the future, so we don't roll
-- back a transction with no statements.
box.commit();
box.begin() s:insert{1, 'Must be rolled back'};
-- nothing - the transaction was rolled back
while s:get{1} ~= nil do fiber.sleep(0) end
-- nothing to commit because of yield
box.commit();
-- Test background fiber
--
function sloppy()
    box.begin()
    s:insert{1, 'From background fiber'}
end;
f = fiber.create(sloppy);
while f:status() == 'running' do
    fiber.sleep(0)
end;
-- When the sloppy fiber ends, its session has an active transction
-- It's rolled back automatically
s:select{};
t = nil;
function sloppy()
    box.begin()
    s:insert{1, 'From background fiber'}
    fiber.sleep(0)
    pcall(box.commit)
    t = s:select{}
end;
f = fiber.create(sloppy);
while f:status() ~= 'dead' do
    fiber.sleep(0)
end;
t;
s:select{};
s:drop();
test_run:cmd("setopt delimiter ''");
test = box.schema.space.create('test')
tindex = test:create_index('primary')
box.begin() test:insert{1} box.rollback()
test:select{1}
box.begin() test:insert{1} box.commit()
test:select{1}

--
-- Test statement-level rollback
--
box.space.test:truncate()
function insert(a) box.space.test:insert(a) end
test_run:cmd("setopt delimiter ';'")
function dup_key()
    box.begin()
    box.space.test:insert{1}
    local status, _ = pcall(insert, {1})
    if not status then
        if box.error.last().code ~= box.error.TUPLE_FOUND then
            box.error()
        end
        box.space.test:insert{2}
    end
    box.commit()
end;
test_run:cmd("setopt delimiter ''");
dup_key()
box.space.test:select{}
--
-- transaction which uses a non-existing space (used to crash in
-- rollbackStatement)
--
test = box.space.test
box.space.test:drop()
status, message = pcall(function() box.begin() test:put{1} test:put{2} box.commit() end)
status
message:match('does not exist')
if not status then box.rollback() end
test = nil

test_run:cmd("setopt delimiter ';'")
function tx_limit(n)
    box.begin()
    for i=0,n do
        box.space.test:insert{i}
    end
    box.commit()
end;
test_run:cmd("setopt delimiter ''");
_ = box.schema.space.create('test');
_ = box.space.test:create_index('primary');
tx_limit(10000)
box.space.test:len()
box.space.test:drop()

--
-- gh-1638: box.rollback on a JIT-ed code path crashes LuaJIT
-- (ffi call + yield don't mix well, rollback started to yield recently)
-- Note: don't remove gh_1638(), it's necessary to trigger JIT-compilation.
--
function gh_1638() box.begin(); box.rollback() end
for i = 1, 1000 do fiber.create(function() gh_1638() end) end

--
--gh-818 add atomic()
--
space = box.schema.space.create('atomic')
index = space:create_index('primary')
test_run:cmd("setopt delimiter ';'")

function args(...)
    return 'args', ...
end;
box.atomic(args, 1, 2, 3, 4, 5);

function tx()
    space:auto_increment{'first row'}
    space:auto_increment{'second row'}
    return space:select{}
end;
box.atomic(tx);

function tx_error(space)
    space:auto_increment{'third'}
    space:auto_increment{'fourth'}
    error("some error")
end;
box.atomic(tx_error, space);

function nested(space)
    box.begin()
end;
box.atomic(nested, space);

function rollback(space)
    space:auto_increment{'fifth'}
    box.rollback()
end;
box.atomic(rollback, space);

test_run:cmd("setopt delimiter ''");
space:select{}

space:drop()

--
-- gh-3528: sysview engine shouldn't participate in transaction control
--
space = box.schema.space.create('test', {id = 9000})
index = space:create_index('primary')

test_run:cmd("setopt delimiter ';'")
box.begin()
space:auto_increment{box.space._vspace:get{space.id}}
space:auto_increment{box.space._vindex:get{space.id, index.id}}
box.commit();
test_run:cmd("setopt delimiter ''");

space:select()
space:drop()

-- In order to implement a transactional applier, we  lifted a
-- restriction that DDL must always run in autocommit mode. The
-- applier always starts a multi-statement transaction, even for
-- single-statement transactions received from the master. Now
-- we only require a DDL statement to be the first statement in a
-- transaction. We even allow DDL to be followed by DML, since
-- it's harmless: if DML yields, the entire transaction is rolled
-- back by a yield trigger. Test that.
_ = box.schema.space.create('memtx', {engine='memtx'})
_ = box.space.memtx:create_index('pk')
box.space.memtx:insert{1, 'memtx'}
_ = box.schema.space.create('vinyl', {engine='vinyl'})
_ = box.space.vinyl:create_index('pk')
box.space.vinyl:insert{1, 'vinyl'}
_ = box.schema.space.create('test')

test_run:cmd("setopt delimiter ';'")
--
-- Test DDL object creation and deletion should leave no
-- effects after rollback
--
box.begin()
box.space.test:create_index('pk')
box.rollback();
box.space.test.index.pk;
--
--
_ = box.space.test:create_index('pk');
box.space.test:insert{1}

box.begin()
box.space.test:truncate()
box.rollback();
box.space.test:select{};
--
--
-- DDL followed by a select from memtx space. Such
-- a select doesn't yield.
--
box.begin()
box.space.test:truncate()
box.space.memtx:select{}
box.commit();

-- DDL and select from a vinyl space. Multi-engine transaction
-- is not allowed.
box.begin()
box.space.test:truncate()
box.space.vinyl:select{}

-- A transaction is left open due to an exception in the Lua fragment
box.rollback();

-- DDL as a second statement - doesn't work
box.begin()
box.space.memtx:insert{2, 'truncate'}
box.space.test:truncate();

-- A transaction is left open due to an exception in the Lua fragment
box.rollback();

box.space.memtx:select{};

-- DML as a second statement - works if the engine is the same
box.begin()
box.space.test:truncate()
box.space.memtx:insert{2, 'truncate'}
box.commit();

box.space.memtx:select{};

-- DML as a second statement - works if the engine is the same
box.begin()
box.space.test:truncate()
box.space.vinyl:insert{2, 'truncate'};

-- A transaction is left open due to an exception in the above fragment
box.rollback();

box.space.vinyl:select{};

-- Two DDL satements in a row
box.begin()
box.space.test:truncate()
box.space.test:truncate();

-- A transaction is left open due to an exception in the above fragment
box.rollback();

-- Two DDL stateemnts on different engines
box.begin()
box.space.memtx:truncate()
box.space.vinyl:truncate();

box.rollback();

box.space.memtx:select{};
box.space.vinyl:select{};

-- cleanup
test_run:cmd("setopt delimiter ''");

if box.space.test then box.space.test:drop() end
box.space.memtx:drop()
box.space.vinyl:drop()

