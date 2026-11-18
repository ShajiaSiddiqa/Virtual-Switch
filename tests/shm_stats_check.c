#include "shm_stats.h"

#include <assert.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
    StatsHandle owner;
    assert(shm_stats_create(&owner, 2) == 0);

    pid_t child = fork();
    assert(child >= 0);

    if (child == 0) {
        StatsHandle client;
        assert(shm_stats_attach(&client) == 0);

        for (int i = 0; i < 10000; i++) {
            assert(shm_stats_add(&client, 0, STATS_FRAMES_IN) == 0);
        }

        assert(shm_stats_close(&client) == 0);
        _exit(0);
    }

    for (int i = 0; i < 10000; i++) {
        assert(shm_stats_add(&owner, 0, STATS_FRAMES_IN) == 0);
    }

    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    PortStats snapshot[SHM_STATS_MAX_PORTS];
    size_t num_ports;

    assert(shm_stats_snapshot(&owner, snapshot,
                               SHM_STATS_MAX_PORTS, &num_ports) == 0);
    assert(num_ports == 2);
    assert(snapshot[0].frames_in == 20000);
    assert(snapshot[1].frames_in == 0);

    assert(shm_stats_close(&owner) == 0);
    puts("PASS: two processes shared exactly 20000 counter increments");
    return 0;
}