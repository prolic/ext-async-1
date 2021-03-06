--TEST--
TCP slow receiver.
--SKIPIF--
<?php require __DIR__ . '/skipif.inc'; ?>
--FILE--
<?php

namespace Concurrent\Network;

use Concurrent\Task;
use Concurrent\Timer;

list ($a, $b) = TcpSocket::pair();

Task::async(function () use ($a) {
    try {
        $timer = new Timer(2);
        $len = 0;
    
        while (null !== ($chunk = $a->read())) {
            $timer->awaitTimeout();
            $len += strlen($chunk);
            
            if ($chunk !== str_repeat('A', strlen($chunk))) {
            	throw new \Error('Corrupted data received');
            }
        }
        
        var_dump($len);
    } catch (\Throwable $e) {
        echo $e, "\n\n";
    } finally {
        $a->close();
    }
});

try {
    $b->setOption(TcpSocket::NODELAY, true);

    $chunk = str_repeat('A', 7000);

    for ($i = 0; $i < 1000; $i++) {
        $b->write($chunk);
    }
} finally {
    $b->close();
}

--EXPECT--
int(7000000)
