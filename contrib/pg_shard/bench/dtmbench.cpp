#include <time.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <inttypes.h>
#include <sys/time.h>
#include <pthread.h>

#include <string>
#include <vector>

#include <pqxx/connection>
#include <pqxx/transaction>
#include <pqxx/nontransaction>
#include <pqxx/pipeline>

using namespace std;
using namespace pqxx;

typedef void* (*thread_proc_t)(void*);
typedef uint32_t xid_t;

struct thread
{
    pthread_t t;
    size_t transactions;
    size_t updates;
    size_t selects;
    size_t aborts;
    int id;

    void start(int tid, thread_proc_t proc) { 
        id = tid;
        updates = 0;
        selects = 0;
        aborts = 0;
        transactions = 0;
        pthread_create(&t, NULL, proc, this);
    }

    void wait() { 
        pthread_join(t, NULL);
    }
};

struct config
{
    int nReaders;
    int nWriters;
    int nIterations;
    int nAccounts;
    int updatePercent;
    string connection;

    config() {
        nReaders = 1;
        nWriters = 10;
        nIterations = 1000;
        nAccounts = 10000;
        updatePercent = 100;
    }
};

config cfg;
bool running;

#define USEC 1000000

static time_t getCurrentTime()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (time_t)tv.tv_sec*USEC + tv.tv_usec;
}


void exec(transaction_base& txn, char const* sql, ...)
{
    va_list args;
    va_start(args, sql);
    char buf[1024];
    vsprintf(buf, sql, args);
    va_end(args);
    txn.exec(buf);
}

template<class T>
T execQuery( transaction_base& txn, char const* sql, ...)
{
    va_list args;
    va_start(args, sql);
    char buf[1024];
    vsprintf(buf, sql, args);
    va_end(args);
    result r = txn.exec(buf);
    return r[0][0].as(T());
}  

void* reader(void* arg)
{
    thread& t = *(thread*)arg;
	connection conn(cfg.connection);
    int64_t prevSum = 0;

    while (running) {
        work txn(conn);
        result r = txn.exec("select sum(v) from t");
        int64_t sum = r[0][0].as(int64_t());
        if (sum != prevSum) {
            printf("Total=%ld\n", sum);
            prevSum = sum;
        }
        t.transactions += 1;
        t.selects += 1;
        txn.commit();
    }
    return NULL;
}
 
void* writer(void* arg)
{
    thread& t = *(thread*)arg;
    connection conn(cfg.connection);
    for (int i = 0; i < cfg.nIterations; i++)
    { 
		work txn(conn);
        int srcAcc = random() % cfg.nAccounts;
        int dstAcc = random() % cfg.nAccounts;
        try {            
            if (random() % 100 < cfg.updatePercent) { 
                exec(txn, "update t set v = v - 1 where u=%d", srcAcc);
                exec(txn, "update t set v = v + 1 where u=%d", dstAcc);
                t.updates += 2;
            } else { 
                int64_t sum = execQuery<int64_t>(txn, "select v from t where u=%d", srcAcc)
                    + execQuery<int64_t>(txn, "select v from t where u=%d", dstAcc);
                if (sum > cfg.nIterations*cfg.nWriters || sum < -cfg.nIterations*cfg.nWriters) { 
                    printf("Wrong sum=%ld\n", sum);
                }
                t.selects += 2;
            }
            txn.commit();            
            t.transactions += 1;
        } catch (pqxx_exception const& x) { 
            txn.abort();
            t.aborts += 1;
            i -= 1;
            continue;
        }
    }
    return NULL;
}
      
void initializeDatabase()
{
    connection conn(cfg.connection);
    work txn(conn);
	exec(txn, "CREATE EXTENSION pg_shard");
	exec(txn, "create table t(u int primary key, v int)");
	exec(txn, "SELECT master_create_distributed_table(table_name := 't', partition_column := 'u')");
	exec(txn, "SELECT master_create_worker_shards(table_name := 't', shard_count := 100, replication_factor := 1)");
	for (int i = 0; i < cfg.nAccounts; i++) { 
		exec(txn, "insert into t values (%d,0)", i);
	}
    txn.commit();
}

int main (int argc, char* argv[])
{
    bool initialize = false;

    if (argc == 1){
        printf("Use -h to show usage options\n");
        return 1;
    }

    for (int i = 1; i < argc; i++) { 
        if (argv[i][0] == '-') { 
            switch (argv[i][1]) { 
            case 'r':
                cfg.nReaders = atoi(argv[++i]);
                continue;
            case 'w':
                cfg.nWriters = atoi(argv[++i]);
                continue;                
            case 'a':
                cfg.nAccounts = atoi(argv[++i]);
                continue;
            case 'n':
                cfg.nIterations = atoi(argv[++i]);
                continue;
            case 'p':
                cfg.updatePercent = atoi(argv[++i]);
                continue;
            case 'c':
                cfg.connection = string(argv[++i]);
                continue;
            case 'i':
                initialize = true;
                continue;
            }
        }
        printf("Options:\n"
               "\t-r N\tnumber of readers (1)\n"
               "\t-w N\tnumber of writers (10)\n"
               "\t-a N\tnumber of accounts (100000)\n"
               "\t-n N\tnumber of iterations (1000)\n"
               "\t-p N\tupdate percent (100)\n"
               "\t-c STR\tdatabase connection string\n"
               "\t-i\tinitialize database\n");
        return 1;
    }

    if (initialize) { 
        initializeDatabase();
        printf("%d accounts inserted\n", cfg.nAccounts);
        return 0;
    }

    time_t start = getCurrentTime();
    running = true;

    vector<thread> readers(cfg.nReaders);
    vector<thread> writers(cfg.nWriters);
    size_t nAborts = 0;
    size_t nUpdates = 0;
    size_t nSelects = 0;
    size_t nTransactions = 0;

    for (int i = 0; i < cfg.nReaders; i++) { 
        readers[i].start(i, reader);
    }
    for (int i = 0; i < cfg.nWriters; i++) { 
        writers[i].start(i, writer);
    }
    
    for (int i = 0; i < cfg.nWriters; i++) { 
        writers[i].wait();
        nUpdates += writers[i].updates;
        nSelects += writers[i].selects;
        nAborts += writers[i].aborts;
        nTransactions += writers[i].transactions;
    }
    
    running = false;

    for (int i = 0; i < cfg.nReaders; i++) { 
        readers[i].wait();
        nSelects += readers[i].selects;
        nTransactions += writers[i].transactions;
    }
 
    time_t elapsed = getCurrentTime() - start;

    printf(
        "{\"tps\":%f, \"transactions\":%ld,"
        " \"selects\":%ld, \"updates\":%ld, \"aborts\":%ld, \"abort_percent\": %d,"
        " \"readers\":%d, \"writers\":%d, \"update_percent\":%d, \"accounts\":%d, \"iterations\":%d}\n",
        (double)(nTransactions*USEC)/elapsed,
        nTransactions,
        nSelects, 
        nUpdates,
        nAborts,
        (int)(nAborts*100/nTransactions),        
        cfg.nReaders,
        cfg.nWriters,
        cfg.updatePercent,
        cfg.nAccounts,
        cfg.nIterations);

    return 0;
}
