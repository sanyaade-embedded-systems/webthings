## TD_COAP_CORE_04
##
## description: Perform DELETE transaction (CON mode)
## status: complete,tested

. ../share/common.sh

#
# Init
#
t_init
t_srv_run

#
# Step 1
#
t_dbg "# Step 1"

t_cli_set_type CON
t_cli_set_method DELETE

out=`t_cli_run`

#
# Step 2
#
t_dbg "# Step 2"

t_field_check 1 srv T CON
t_field_check 1 srv Code DELETE

#
# Step 3
#
t_dbg "# Step 3"

t_field_check 1 cli Code "2.02 (Deleted)"
v=`t_field_get 1 srv MID`
t_field_check 1 cli MID "${v}"

#
# Step 4
#
t_dbg "# Step 4"

t_dbg "${out}"
if [ "${EC_PLUG_MODE}" != "srv" ]; then
    t_cmp "${out}" "Hello world!"
fi

#
# Cleanup
#
t_term