--TEST--
Task can deal with exit within await.
--SKIPIF--
<?php require __DIR__ . '/skipif.inc'; ?>
--FILE--
<?php

namespace Concurrent;

$t = Task::async(function () {
    (new Timer(50))->awaitTimeout();

    var_dump('BEFORE EXIT');
    
    try {
        exit();
    } finally {
    	var_dump('AFTER EXIT');
    }
});

(new Timer(20))->awaitTimeout();

var_dump('BEFORE AWAIT');

try {
    Task::await($t);
} finally {
    var_dump('AFTER AWAIT');
}


?>
--EXPECT--
string(12) "BEFORE AWAIT"
string(11) "BEFORE EXIT"
