env = require('test_run')
test_run = env.new()
test_run:cmd("push filter ".."'\\.lua.*:[0-9]+: ' to '.lua...\"]:<line>: '")
engine = test_run:get_cfg('engine')
box.execute('pragma sql_default_engine=\''..engine..'\'')

--
-- gh-3272: Move SQL CHECK into server
--

-- Until Tarantool version 2.2 check constraints were stored in
-- space opts.
-- Make sure that now this legacy option is ignored.
opts = {checks = {{expr = 'X>5'}}}
format = {{name = 'X', type = 'unsigned'}}
t = {513, 1, 'test', 'memtx', 0, opts, format}
s = box.space._space:insert(t)
_ = box.space.test:create_index('pk')

-- Invalid expression test.
box.space._ck_constraint:insert({'CK_CONSTRAINT_01', 513, false, 'X><5', 'SQL'})
-- Unexistent space test.
box.space._ck_constraint:insert({'CK_CONSTRAINT_01', 550, false, 'X<5', 'SQL'})
-- Pass integer instead of expression.
box.space._ck_constraint:insert({'CK_CONSTRAINT_01', 513, false, 666, 'SQL'})
-- Defered CK constraints are not supported.
box.space._ck_constraint:insert({'CK_CONSTRAINT_01', 513, true, 'X<5', 'SQL'})
-- The only supperted language is SQL.
box.space._ck_constraint:insert({'CK_CONSTRAINT_01', 513, false, 'X<5', 'LUA'})

-- Check constraints LUA creation test.
box.space._ck_constraint:insert({'CK_CONSTRAINT_01', 513, false, 'X<5', 'SQL'})
box.space._ck_constraint:count({})

box.execute("INSERT INTO \"test\" VALUES(5);")
box.space.test:insert({5})
box.space._ck_constraint:replace({'CK_CONSTRAINT_01', 513, false, 'X<=5', 'SQL'})
box.execute("INSERT INTO \"test\" VALUES(5);")
box.execute("INSERT INTO \"test\" VALUES(6);")
box.space.test:insert({6})
-- Can't drop table with check constraints.
box.space.test:delete({5})
box.space.test.index.pk:drop()
box.space._space:delete({513})
box.space._ck_constraint:delete({'CK_CONSTRAINT_01', 513})
box.space._space:delete({513})

-- Create table with checks in sql.
box.execute("CREATE TABLE t1(x INTEGER CONSTRAINT ONE CHECK( x<5 ), y REAL CONSTRAINT TWO CHECK( y>x ), z INTEGER PRIMARY KEY);")
box.space._ck_constraint:count()
box.execute("INSERT INTO t1 VALUES (7, 1, 1)")
box.space.T1:insert({7, 1, 1})
box.execute("INSERT INTO t1 VALUES (2, 1, 1)")
box.space.T1:insert({2, 1, 1})
box.execute("INSERT INTO t1 VALUES (2, 4, 1)")
box.space.T1:update({1}, {{'+', 1, 5}})
box.execute("DROP TABLE t1")

-- Test space creation rollback on spell error in ck constraint.
box.execute("CREATE TABLE first (id FLOAT PRIMARY KEY CHECK(id < 5), a INT CONSTRAINT ONE CHECK(a >< 5));")
box.space.FIRST == nil
box.space._ck_constraint:count() == 0

-- Ck constraints are disallowed for spaces having no format.
s = box.schema.create_space('test', {engine = engine})
_ = s:create_index('pk')
_ = box.space._ck_constraint:insert({'physics', s.id, false, 'X<Y', 'SQL'})
s:format({{name='X', type='integer'}, {name='Y', type='integer'}})
_ = box.space._ck_constraint:insert({'physics', s.id, false, 'X<Y', 'SQL'})
box.execute("INSERT INTO \"test\" VALUES(2, 1);")
s:format({{name='Y', type='integer'}, {name='X', type='integer'}})
box.execute("INSERT INTO \"test\" VALUES(1, 2);")
box.execute("INSERT INTO \"test\" VALUES(2, 1);")
s:truncate()
box.execute("INSERT INTO \"test\" VALUES(1, 2);")
s:format({})
s:format()
s:format({{name='Y1', type='integer'}, {name='X1', type='integer'}})
-- Ck constraint creation is forbidden for non-empty space
s:insert({2, 1})
_ = box.space._ck_constraint:insert({'conflict', s.id, false, 'X>10', 'SQL'})
s:truncate()
_ = box.space._ck_constraint:insert({'conflict', s.id, false, 'X>10', 'SQL'})
box.execute("INSERT INTO \"test\" VALUES(1, 2);")
box.execute("INSERT INTO \"test\" VALUES(11, 11);")
box.execute("INSERT INTO \"test\" VALUES(12, 11);")
s:drop()

box.execute("CREATE TABLE T2(ID INT PRIMARY KEY, CONSTRAINT CK1 CHECK(ID > 0), CONSTRAINT CK1 CHECK(ID < 0))")
box.space._ck_constraint:select()

--
-- gh-3611: Segfault on table creation with check referencing this table
--
box.execute("CREATE TABLE w2 (s1 INT PRIMARY KEY, CHECK ((SELECT COUNT(*) FROM w2) = 0));")
box.execute("DROP TABLE w2;")

--
-- gh-3653: Dissallow bindings for DDL
--
box.execute("CREATE TABLE t5(x INT PRIMARY KEY, y INT, CHECK( x*y < ? ));")

--
-- Test binding reset on new insertion
--
s = box.schema.create_space('test', {engine = engine})
_ = s:create_index('pk')
s:format({{name='X', type='any'}, {name='Y', type='integer'}, {name='Z', type='integer', is_nullable=true}})
ck_not_null = box.space._ck_constraint:insert({'ZnotNULL', s.id, false, 'X = 1 AND Z IS NOT NULL', 'SQL'})
s:insert({2, 1})
s:insert({1, 1})
s:insert({1, 1, box.NULL})
s:insert({2, 1, 3})
s:insert({1, 1})
s:insert({1, 1, 3})
s:drop()

--
-- Test ck constraint corner cases
--
s = box.schema.create_space('test', {engine = engine})
_ = s:create_index('pk')
s:format({{name='X', type='any'}, {name='Y', type='integer'}, {name='Z', type='integer', is_nullable=true}})
ck_not_null = box.space._ck_constraint:insert({'ZnotNULL', s.id, false, 'Z IS NOT NULL', 'SQL'})
s:insert({1, 2, box.NULL})
s:insert({1, 2})
_ = box.space._ck_constraint:delete({'ZnotNULL', s.id})
_ = box.space._ck_constraint:insert({'XlessY', s.id, false, 'X < Y and Y < Z', 'SQL'})
s:insert({'1', 2})
s:insert({})
s:insert({2, 1})
s:insert({1, 2})
s:insert({2, 3, 1})
s:insert({2, 3, 4})
s:update({2}, {{'+', 2, 3}})
s:update({2}, {{'+', 2, 3}, {'+', 3, 3}})
s:replace({2, 1, 3})
box.snapshot()
s = box.space["test"]
s:update({2}, {{'+', 2, 3}})
s:update({2}, {{'+', 2, 3}, {'+', 3, 3}})
s:replace({2, 1, 3})
s:drop()

--
-- Test complex CHECK constraints.
--
s = box.schema.create_space('test', {engine = engine})
s:format({{name='X', type='integer'}, {name='Y', type='integer'}, {name='Z', type='integer'}})
_ = s:create_index('pk', {parts = {3, 'integer'}})
_ = s:create_index('unique', {parts = {1, 'integer'}})
_ = box.space._ck_constraint:insert({'complex1', s.id, false, 'x+y==11 OR x*y==12 OR x/y BETWEEN 5 AND 8 OR -x == y+10', 'SQL'})
s:insert({1, 10, 1})
s:update({1}, {{'=', 1, 4}, {'=', 2, 3}})
s:update({1}, {{'=', 1, 12}, {'=', 2, 2}})
s:update({1}, {{'=', 1, 12}, {'=', 2, -22}})
s:update({1}, {{'=', 1, 0}, {'=', 2, 1}})
s:get({1})
s:update({1}, {{'=', 1, 0}, {'=', 2, 2}})
s:get({1})
s:drop()

s = box.schema.create_space('test', {engine = engine})
s:format({{name='X', type='integer'}, {name='Z', type='any'}})
_ = s:create_index('pk', {parts = {1, 'integer'}})
_ = box.space._ck_constraint:insert({'complex2', s.id, false, 'typeof(coalesce(z,0))==\'integer\'', 'SQL'})
s:insert({1, 'string'})
s:insert({1, {map=true}})
s:insert({1, {'a', 'r','r','a','y'}})
s:insert({1, 3.14})
s:insert({1, 666})
s:drop()

--
-- Test binding optimisation.
--
s = box.schema.create_space('test')
_ = s:create_index('pk', {parts = {1, 'integer'}})
format65 = {}
test_run:cmd("setopt delimiter ';'")
for i = 1,66 do
        table.insert(format65, {name='X'..i, type='integer', is_nullable = true})
end
test_run:cmd("setopt delimiter ''");
s:format(format65)
_ = box.space._ck_constraint:insert({'X1is666andX65is666', s.id, false, 'X1 == 666 and X65 == 666 and X63 IS NOT NULL', 'SQL'})
s:insert(s:frommap({X1 = 1, X65 = 1}))
s:insert(s:frommap({X1 = 666, X65 = 1}))
s:insert(s:frommap({X1 = 1, X65 = 666}))
s:insert(s:frommap({X1 = 666, X65 = 666}))
s:insert(s:frommap({X1 = 666, X65 = 666, X63 = 1}))
s:drop()

test_run:cmd("clear filter")
