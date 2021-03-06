#include <iostream>
#include <omp.h>
#include <mpi.h>
#include <pthread.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <algorithm>
using namespace std;

int PROCESS_CHUNK_SIZE;
int THREAD_CHUNK_SIZE = 20;
#define MAX_ITER 100000

clock_t time_bg, time_ed;
double time_dur;
MPI_File out;
int tot_size, num_process, master_cur, master_num_actprocess;
pthread_mutex_t mutex;

int cardioid_check(double x, double y){
    double q = ((x - 0.25) * (x - 0.25) + y * y);
    return q * (q + (x - 0.25)) < 0.25 * y * y || ((x + 1) * (x + 1) + y * y < 1.0 / 16);
}

int compute(double realaxis_left, double realaxis_right, 
            double imageaxis_lower, double imageaxis_upper,
            int width, int height, int i, int j){
    double z_real = 0.0;
    double z_imag = 0.0;
    double c_real = realaxis_left + (double)i * ((realaxis_right - realaxis_left) / (double)width); 
    double c_imag = imageaxis_lower + (double)j * ((imageaxis_upper - imageaxis_lower) / (double)height);
    int repeats = 0;
    double lengthsq = 0.0;

    double real_temp, imag_temp;

    if (cardioid_check(c_real, c_imag))
        return MAX_ITER;

    while (repeats < MAX_ITER && lengthsq <= 4.0){
        real_temp = z_real * z_real - z_imag * z_imag + c_real;
        imag_temp = 2 * z_real * z_imag + c_imag;
        z_real = real_temp;
        z_imag = imag_temp;
        lengthsq = z_real * z_real + z_imag * z_imag;
        repeats ++;
    }

    return repeats;
}


void *master_schedule(void *arg){
    int* buf = new int[2];
    int* master_buf = new int[2];
    int master_source = 0;
    if (master_num_actprocess == 0) pthread_exit(NULL);
    do{
        MPI_Recv(master_buf, 1, MPI_INT, MPI_ANY_SOURCE, 1, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        master_num_actprocess --;
        master_source = master_buf[0];

        pthread_mutex_lock(&mutex);
        if (master_cur < tot_size){
            master_buf[0] = min(tot_size - master_cur, PROCESS_CHUNK_SIZE);
            master_buf[1] = master_cur;
            MPI_Send(master_buf, 2, MPI_INT, master_source, 0, MPI_COMM_WORLD);
            master_num_actprocess ++;
            master_cur += min(tot_size - master_cur, PROCESS_CHUNK_SIZE);
        }
        else{
            master_buf[0] = 0;
            MPI_Send(master_buf, 2, MPI_INT, master_source, 0, MPI_COMM_WORLD);
        }
        pthread_mutex_unlock(&mutex);

    }while (master_num_actprocess > 0);
    pthread_exit(NULL);
}


int main(int argc, char* argv[])
{
    int thread_num = atoi(argv[1]); 
    double realaxis_left = atof(argv[2]);
    double realaxis_right = atof(argv[3]);
    double imageaxis_lower = atof(argv[4]);
    double imageaxis_upper = atof(argv[5]);
    int xpoint = atoi(argv[6]);
    int ypoint = atoi(argv[7]);
    char *output_filename = argv[8];

    /* set window size */
    int width = xpoint;
    int height = ypoint;
    PROCESS_CHUNK_SIZE = (width + height) * 5;

    tot_size = width * height;

    int rank, size;

    MPI_Init(&argc, &argv);
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Request req;
    MPI_Request req_s;
    pthread_mutex_init (&mutex, NULL);

    int* buf = new int[2];
    int* master_buf = new int[2];
    int* sbuf = new int[1];
    num_process = size;
    int comp_x = 0, comp_y = 0, comp_id;

    int* ans_buf = new int[tot_size];
    int ans_curi = 0;
    int ans_curl = 0;
    int* ans_rk = new int[tot_size / PROCESS_CHUNK_SIZE + 1];
    int* ans_bg = new int[tot_size / PROCESS_CHUNK_SIZE + 1];
    int* ans_len = new int[tot_size / PROCESS_CHUNK_SIZE + 1];

    int oerr = MPI_File_open(MPI_COMM_WORLD, output_filename, 
        MPI_MODE_CREATE|MPI_MODE_WRONLY, MPI_INFO_NULL, &out);
    if (oerr) {
        if (rank == 0)
            fprintf(stderr, "%s: Couldn't open file %s\n",
                argv[0], argv[3]);
        MPI_Finalize();
        exit(2);
    }
    master_cur = 0;
    master_num_actprocess = 0;

    if (rank == 0){
        MPI_Offset global_start = 0;
        buf[0] = width;
        buf[1] = height;
        MPI_File_seek(out, global_start, MPI_SEEK_SET);
        MPI_File_write(out, buf, 2, MPI_INT, MPI_STATUS_IGNORE);

        int i;
        for (i = 1; i < num_process && master_cur < tot_size; ++i){
            buf[1] = master_cur;
            buf[0] = min(tot_size - master_cur, PROCESS_CHUNK_SIZE);
            MPI_Isend(buf, 2, MPI_INT, i, 0, MPI_COMM_WORLD, &req);
            master_num_actprocess ++;
            master_cur += buf[0];
        }
        for (; i < num_process; ++i){
            buf[1] = 0;
            buf[0] = 0;
            MPI_Isend(buf, 2, MPI_INT, i, 0, MPI_COMM_WORLD, &req);
        }
    }
    MPI_Barrier(MPI_COMM_WORLD);

    if (rank == 0){
        pthread_t master_thd;
        pthread_create(&master_thd, NULL, master_schedule, NULL);
        ans_rk[ans_curi] = 0;
        ans_bg[ans_curi] = tot_size;
        ans_len[ans_curi] = 0;

        int cur_len = 0;
        while (1){
            pthread_mutex_lock(&mutex);
            if (master_cur < tot_size){
                cur_len = min(tot_size - master_cur, PROCESS_CHUNK_SIZE);
                tot_size -= cur_len;
            }
            else cur_len = 0;
            pthread_mutex_unlock(&mutex);
            if (cur_len == 0)
                break;

            ans_curi = 1;
            int bf_bg = ans_bg[0] - cur_len;

            #pragma omp parallel num_threads(thread_num)
            {
                #pragma omp for schedule(dynamic, THREAD_CHUNK_SIZE) private(comp_id, comp_x, comp_y)
                for (int i = 0; i < cur_len; ++i){
                    comp_id = bf_bg + i;
                    comp_x = comp_id / height;
                    comp_y = comp_id % height;
                    ans_buf[ans_len[0] + cur_len - i - 1] = compute(realaxis_left, realaxis_right, imageaxis_lower, imageaxis_upper,
                                                            width, height, comp_x, comp_y);
                }
            }
            

            ans_bg[0] = bf_bg;
            ans_len[0] += cur_len;
        }
        reverse(ans_buf, ans_buf + ans_len[0]);
        pthread_join(master_thd, NULL);
    }
    else{
        while (1){
            MPI_Recv(buf, 2, MPI_INT, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            if (buf[0] == 0)
                break;

            ans_rk[ans_curi] = ans_curl;
            ans_bg[ans_curi] = buf[1];
            ans_len[ans_curi] = buf[0];
            ans_curi += 1;

            #pragma omp parallel num_threads(thread_num)
            {
                #pragma omp for schedule(dynamic, THREAD_CHUNK_SIZE) private(comp_id, comp_x, comp_y)
                for (int i = 0; i < buf[0]; ++i){
                    comp_id = buf[1] + i;
                    comp_x = comp_id / height;
                    comp_y = comp_id % height;
                    ans_buf[ans_curl + i] = compute(realaxis_left, realaxis_right, imageaxis_lower, imageaxis_upper,
                                                    width, height, comp_x, comp_y);
                }
            }

            ans_curl += buf[0];
            sbuf[0] = rank;

            MPI_Send(sbuf, 1, MPI_INT, 0, 1, MPI_COMM_WORLD);
        }
    }

    for (int i = 0; i < ans_curi; ++i){
        MPI_Offset global_start = ans_bg[i] + 2;
        global_start = global_start * sizeof(int);
        MPI_File_seek(out, global_start, MPI_SEEK_SET);
        MPI_File_write(out, ans_buf + ans_rk[i], ans_len[i], MPI_INT, MPI_STATUS_IGNORE);
    }

    MPI_File_close(&out);
    MPI_Finalize();
}


