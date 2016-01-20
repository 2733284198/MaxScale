#!/bin/bash

#bug 454
rp=`realpath $0`
export test_dir=`dirname $rp`
export test_name=`basename $rp`

$test_dir/configure_maxscale.sh

$test_dir/session_hang/run_setmix.sh &
perl $test_dir/session_hang/simpletest.pl

sleep 15

$test_dir/session_hang/run_setmix.sh &
perl $test_dir/session_hang/simpletest.pl

sleep 15

$test_dir/session_hang/run_setmix.sh &
perl $test_dir/session_hang/simpletest.pl

sleep 15

$test_dir/session_hang/run_setmix.sh &
perl $test_dir/session_hang/simpletest.pl

sleep 15


echo "show databases;" mysql -u$repl_user -p$repl_password -h$maxscale_IP -p 4006
res=$?
$test_dir/copy_logs.sh run_session_hang

exit $res
