<?php

$fp = stream_socket_client("tcp://127.0.0.1:8080", $errno, $errstr);
if (!$fp) {
  echo "errno: $errno - $errstr\n";
  exit(-1);
}
while( ($stream = fread($fp, 1024)) !== false) {
  echo $stream, "\n";

  fputs(STDOUT, "[input]:");
  $str = fgets(STDIN, 1024);
  if (strlen($str) > 0) {
    fwrite($fp, $str);
  }
}

fclose($fp);