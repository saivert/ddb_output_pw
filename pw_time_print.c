        struct pw_time time;
        pw_stream_get_time_n(data->stream, &time, sizeof(time));
        printf("now %ld, rate %u/%u, ticks %lu, delay %ld, queued %lu, buffered %lu, queued_buffers %u, avail_buffers %u\r",
        time.now, time.rate.num ,time.rate.denom, time.ticks, time.delay, time.queued, time.buffered, time.queued_buffers, time.avail_buffers);
        fflush(stdout);
