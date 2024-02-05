<?php

$fp = stream_socket_client("tcp://127.0.0.1:10086", $errno, $errstr);
if (!$fp) {
  echo "errro: $errno - $errstr\n";
  exit(-1);
}

for ($i = 0; $i < 1000; $i ++) {
  fwrite($fp, "hello world!");
}

sleep(1);

fclose($fp);