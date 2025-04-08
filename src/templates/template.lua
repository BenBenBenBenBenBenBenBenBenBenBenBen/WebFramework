local kv_list = {}

local sqlite3 = require("lsqlite3")

local database_name = "ex1"

-- Open (or create) a database
local db = sqlite3.open(database_name)

local create_table = [[
    CREATE TABLE IF NOT EXISTS tbl1 (
        f1 VARCHAR(30) PRIMARY KEY,
        f2 VARCHAR(300)
    );
]]

db:exec(create_table)

-- Execute a simple query
for row in db:nrows("SELECT f1, f2 FROM tbl1") do
    kv_list[row.f1] = row.f2
end

kv_list["die"] = "Matt";

-- Close the database
db:close()

if send_database_name then
    send_database_name(database_name)
end

if send_kv_list then
    send_kv_list(kv_list)
end
