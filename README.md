# ngx_http_var_limit_conn_module

This is a derived module from [ngx_http_limit_conn_module](http://nginx.org/en/docs/http/ngx_http_limit_conn_module.html).
It allows override values of conn, dry_run, and status with variables.
It also provides two directives for monitoring.

## Table of Contents

* [Example Configuration](#example-configuration)
* [Directives](#directives)
    * [var_limit_conn](#var_limit_conn)
    * [var_limit_conn_dry_run](#var_limit_conn_dry_run)
    * [var_limit_conn_log_level](#var_limit_conn_log_level)
    * [var_limit_conn_status](#var_limit_conn_status)
    * [var_limit_conn_zone](#var_limit_conn_zone)
    * [var_limit_conn_top](#var_limit_conn_top)
    * [var_limit_conn_monitor](#var_limit_conn_monitor)
* [Embedded Variables](#embedded-variables)

## Example Configuration

```
http {
    map $host $host_group {
        default            1;
        www1.example.com   2;
    }

    map $request_method $method_group {
        default            B;
        GET                A;
        HEAD               A;
        OPTIONS            A;
    }

    map $host_group-$method_group $limit_conn_by_host_conn {
        default       unlimited;
        1-A           unlimited;
        1-B           50;
        2-A           20;
        2-B            2;
    }

    map $host_group $limit_conn_by_host_status {
        default       429;
        2             444;
    }

    var_limit_conn_zone $host-$method_group zone=limit_conn_by_host_zone:10m
        conn_var=$limit_conn_by_host_conn status_var=$limit_conn_by_host_status;

    ...

    server {

        ...

        location /download/ {
            var_limit_conn addr 1;
        }
    }

    server {
        listen       81;
        server_name  localhost;

        allow 127.0.0.1;
        deny  all;

        location = /top-limit-conn {
            var_limit_conn_top limit_conn_by_host_zone default_n=3;
        }

        location = /status-limit-conn {
            var_limit_conn_monitor limit_conn_by_host_zone;
        }
    }
}    
```

## Directives

### var_limit_conn

* Syntax: **var_limit_conn** *zone* *number*;
* Default: -
* Context: http, server, location

### var_limit_conn_dry_run

* Syntax: **var_limit_conn_dry_run** on | off;
* Default: var_limit_conn_dry_run off;
* Context: http, server, location

### var_limit_conn_log_level

* Syntax: **var_limit_conn_log_level** info | notice | warn | error;
* Default: var_limit_conn_log_level error;
* Context: http, server, location

### var_limit_conn_status

* Syntax:  **var_limit_conn_status** *code*;
* Default: var_limit_conn_status 503;
* Context: http, server, location

### var_limit_conn_zone

* Syntax: **var_limit_conn_zone** *key* zone=*name:size* conn_var=*$conn* dry_run_var=*$dry_run* status_var=*$status*;
* Default: -
* Context: http

### var_limit_conn_top

* Syntax: **var_limit_conn_top** *zone* default_n=*number*;
* Default: -
* Context: location

It shows the top n items in the shared memory zone.
You can override n with a query parameter.

> [!CAUTION]
> Avoid calling this method on dictionaries with a very large number of keys as it may lock the dictionary for significant amount of time and block Nginx worker processes trying to access the dictionary.

The example:

```
$ curl -sSD - 'http://localhost:81/top-limit-conn?n=5'

HTTP/1.1 200 OK
Server: nginx/1.25.4
Date: Sat, 06 Jan 2024 12:46:14 GMT
Content-Type: text/plain
Content-Length: 40
Connection: keep-alive
x-num-all-keys: 1
x-top-n: 1

key:www1.example.com-B  conn:10 limit:10
```

### var_limit_conn_monitor

* Syntax: **var_limit_conn_monitor** *zone*;
* Default: -
* Context: location

It shows the items in the shared memory zone corresponding keys specified with
the comma separated values in the query paramter "key".

The non existing keys are silently skipped and only existing keys are shown.

The example:

```
$ curl -sSD - 'http://localhost:81/status-limit-conn?key=www1.example.com-A,www1.example.com-B'

HTTP/1.1 200 OK
Server: nginx/1.25.4
Date: Sat, 06 Jan 2024 12:46:14 GMT
Content-Type: text/plain
Content-Length: 40
Connection: keep-alive

key:www1.example.com-B  conn:10 limit:10
```

## Embedded Variables

* $limit_conn_status (shared with [ngx_http_limit_conn_module](http://nginx.org/en/docs/http/ngx_http_limit_conn_module.html))
    * keeps the result of limiting the number of connections: `PASSED`, `REJECTED`, or `REJECTED_DRY_RUN`
