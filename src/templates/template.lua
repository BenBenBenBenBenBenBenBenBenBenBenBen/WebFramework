-- A table of key-value pairs
local kv_list = {
    title = "Hello",
    subtitle = "World",
    stuff = "Stuff",
    link = "Shit",
    temp = "perm1",
    hello = "fuck",
    die = "dead"
}

-- Call the C function with the table
send_kv_list(kv_list)
