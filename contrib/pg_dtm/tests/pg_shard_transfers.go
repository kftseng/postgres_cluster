package main

import (
    "fmt"
    "sync"
    _ "github.com/jgallagher/go-libpq"
    "database/sql"
    "strconv"
    "math/rand"
)

const (
    TRANSFER_CONNECTIONS = 8
    INIT_AMOUNT = 10000
    N_ITERATIONS = 10000
    N_ACCOUNTS = 100 //2*TRANSFER_CONNECTIONS
)

var cfg = "host=127.0.0.1 port=5432 sslmode=disable"
var cfg1 = "host=127.0.0.1 port=5433 sslmode=disable"
var cfg2 = "host=127.0.0.1 port=5434 sslmode=disable"

var running = false

func prepare_db() {
    conn1, err := sql.Open("libpq", cfg1)
    checkErr(err)
    exec(conn1, "drop table if exists t_10000")
    conn1.Close()

    conn2, err := sql.Open("libpq", cfg2)
    checkErr(err)
    exec(conn2, "drop table if exists t_10001")
    conn2.Close()


    conn, err := sql.Open("libpq", cfg)
    checkErr(err)

    exec(conn, "drop extension if exists pg_shard CASCADE")
    exec(conn, "create extension pg_shard")
    exec(conn, "drop table if exists t")
    exec(conn, "create table t(u int, v int)")
    exec(conn, "select master_create_distributed_table(table_name := 't', partition_column := 'u')")
    exec(conn, "select master_create_worker_shards(table_name := 't', shard_count := 2, replication_factor := 1)")

    for i:=1; i<=N_ACCOUNTS; i++ {
        exec(conn, "insert into t values(" + strconv.Itoa(i) + ",10000)")
    }

    conn.Close()
}

func transfer(id int, wg *sync.WaitGroup) {
    conn, err := sql.Open("libpq", cfg)
    checkErr(err)
    defer conn.Close()

    uids1 := []int{1,3,4, 5, 7, 8,10,14}
    uids2 := []int{2,6,9,11,12,13,18,21}

    for i:=0; i < N_ITERATIONS; i++ {
        exec(conn, "begin")
        exec(conn, "update t set v = v + 1 where u="+strconv.Itoa(uids1[rand.Intn(TRANSFER_CONNECTIONS)]))
        exec(conn, "update t set v = v - 1 where u="+strconv.Itoa(uids2[rand.Intn(TRANSFER_CONNECTIONS)]))
        // exec(conn, "update t set v = v + 1 where u=1")
        // exec(conn, "update t set v = v - 1 where u=2")
        exec(conn, "commit")

        if i%1000==0 {
            fmt.Printf("%u tx processed.\n", i)
        }
    }

    wg.Done()
}

func inspect(wg *sync.WaitGroup) {
    var sum int64
    var prevSum int64 = 0

    conn, err := sql.Open("libpq", cfg)
    checkErr(err)

    for running {
        sum = execQuery(conn, "select sum(v) from t")
        if sum != prevSum {
            fmt.Println("Total = ", sum);
            prevSum = sum
        }
    }

    conn.Close()
    wg.Done()
}

func main() {
    var transferWg sync.WaitGroup
    var inspectWg sync.WaitGroup

    prepare_db()

    transferWg.Add(TRANSFER_CONNECTIONS)
    for i:=0; i<TRANSFER_CONNECTIONS; i++ {
        go transfer(i, &transferWg)
    }

    running = true
    inspectWg.Add(1)
    go inspect(&inspectWg)

    transferWg.Wait()
    running = false
    
    inspectWg.Wait()

    // conn, err := sql.Open("libpq", cfg)
    // checkErr(err)

    // exec(conn, "begin")
    // sum := execQuery(conn, "select sum(v) from t")
    // exec(conn, "commit")

    // fmt.Println(sum)

    fmt.Printf("done\n")
}

func exec(conn *sql.DB, stmt string) {
    var err error
    _, err = conn.Exec(stmt)
    checkErr(err)
}

func execQuery(conn *sql.DB, stmt string) int64 {
    var err error
    var result int64
    err = conn.QueryRow(stmt).Scan(&result)
    checkErr(err)
    return result
}

func checkErr(err error) {
    if err != nil {
        panic(err)
    }
}


