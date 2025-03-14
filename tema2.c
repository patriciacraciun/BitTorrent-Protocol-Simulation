#include <mpi.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#define TRACKER_RANK 0
#define MAX_FILES 10
#define MAX_FILENAME 15
#define HASH_SIZE 32
#define MAX_CHUNKS 100


typedef struct {
    char hash[HASH_SIZE + 1];
    int index;
} segmentInfo;

typedef struct {
    char filename[MAX_FILENAME];
    segmentInfo segments[MAX_CHUNKS];
    int segments_count;
    int swarm[100];
    int swarm_size;
} FileInfo;


typedef struct {
    char filename[MAX_FILENAME];
    int segments_recv[MAX_CHUNKS];
    int segments_received_count;
    segmentInfo segments[MAX_CHUNKS];
} RequestedFile;

typedef struct {
    int peer_rank;
    int active_requests;
} PeerStatus;

FileInfo files[MAX_FILES];
int files_count = 0;

RequestedFile peer_requested[MAX_FILES];
int peer_requested_count = 0;
int peer_requests_completed = 0;

FileInfo peer_owned_files[MAX_FILES];
int peer_file_count = 0;

PeerStatus peer_status[100];
int peer_status_count = 0;

int numtasks;

int choose_peer_with_least_load(int *peers, int peer_count) {
    if (peer_count == 0) {
        return -1;
    }

    int min_load = INT_MAX;
    int chosen_peer = -1;

    for (int i = 0; i < peer_count; i++) {
        int peer_rank = peers[i];
        int found = 0;
        // cauta peer-ul din lista de peers a fisierului
        for (int j = 0; j < peer_status_count; j++) {
            if (peer_status[j].peer_rank == peer_rank) {
                found = 1;
                if (peer_status[j].active_requests < min_load) {
                    min_load = peer_status[j].active_requests;
                    chosen_peer = peer_rank;
                }
                break;
            }
        }

        if (!found) {
            peer_status[peer_status_count].peer_rank = peer_rank;
            peer_status[peer_status_count].active_requests = 0;
            peer_status_count++;

            if (0 < min_load) {
                min_load = 0;
                chosen_peer = peer_rank;
            }
        }
    }
    // incrementam numarul de cereri pentru peer
    if (chosen_peer != -1) {
        for (int i = 0; i < peer_status_count; i++) {
            if (peer_status[i].peer_rank == chosen_peer) {
                peer_status[i].active_requests++;
                break;
            }
        }
    }

    return chosen_peer;
}


void decrement_peer_load(int peer_rank) {
    for (int i = 0; i < peer_status_count; i++) {
        if (peer_status[i].peer_rank == peer_rank) {
            if (peer_status[i].active_requests > 0) {
                peer_status[i].active_requests--;
            }
            break;
        }
    }
}


void create_output_file(int rank, char *filename, char *hash, int segment_index) {
    char client_filename[MAX_FILENAME];
    snprintf(client_filename, MAX_FILENAME, "client%d_%s", rank, filename);

    FILE *file = fopen(client_filename, "a");
    if (file == NULL) {
        exit(EXIT_FAILURE);
    }

    fprintf(file, "%s\n", hash);

    fclose(file);
}


void request_swarm(char *filename, segmentInfo *segments, int *segments_count, int *swarm, int *swarm_size) {
    // trimite cerere pentru swarm la tracker
    MPI_Send("SWARM REQUEST", 20, MPI_CHAR, TRACKER_RANK, 2, MPI_COMM_WORLD);
    MPI_Send(filename, MAX_FILENAME, MPI_CHAR, TRACKER_RANK, 2, MPI_COMM_WORLD);

    // primeste detaliile despre fisier de la tracker
    MPI_Recv(segments_count, 1, MPI_INT, TRACKER_RANK, 3, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

    for(int i = 0; i < *segments_count; i++) {
        MPI_Recv(segments[i].hash, HASH_SIZE, MPI_CHAR, TRACKER_RANK, 3, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        MPI_Recv(&segments[i].index, 1, MPI_INT, TRACKER_RANK, 3, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        segments[i].hash[32] = '\0';
    }

    MPI_Recv(swarm_size, 1, MPI_INT, TRACKER_RANK, 3, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    MPI_Recv(swarm, *swarm_size, MPI_INT, TRACKER_RANK, 3, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
}


void request_segment(int rank, char *filename, int file_index, segmentInfo *segments, char *hash, int segment_index, int *swarm, int swarm_size) {
    int chosen_seed = choose_peer_with_least_load(swarm, swarm_size);

    // trimite cerere pentru un segment catre peer-ul ales
    MPI_Send("SEGMENT REQUEST", 20, MPI_CHAR, chosen_seed, 1, MPI_COMM_WORLD);
    MPI_Send(peer_requested[file_index].filename, MAX_FILENAME, MPI_CHAR, chosen_seed, 1, MPI_COMM_WORLD);
    MPI_Send(&segments[segment_index].index, 1, MPI_INT, chosen_seed, 1, MPI_COMM_WORLD);

    memset(hash, 0, HASH_SIZE + 1);
    MPI_Recv(hash, HASH_SIZE, MPI_CHAR, chosen_seed, 4, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    hash[HASH_SIZE] = '\0';
    char ack[4];
    MPI_Recv(ack, 4, MPI_CHAR, chosen_seed, 5, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

    // eroare daca ack-ul nu este corect
    if (strcmp(ack, "ACK")) {
        MPI_Abort(MPI_COMM_WORLD, 1);
    }

    decrement_peer_load(chosen_seed);
    // salvam detaliile segmentului primit
    strcpy(peer_requested[file_index].segments[segment_index].hash, hash);
    peer_requested[file_index].segments_recv[segment_index] = chosen_seed;
    peer_requested[file_index].segments[segment_index].index = segment_index;
    peer_requested[file_index].segments_received_count++;
}

// descarca segmentele unui fisier solicitat de peer
void process_requested_file_by_peer(int rank, char *filename, int file_index) {
    segmentInfo segments[MAX_CHUNKS];
    int segments_count = 0;

    int swarm[100];
    int swarm_size = 0;

    // cere informatiile despre swarm-ul corespunzator fisierului
    request_swarm(filename, segments, &segments_count, swarm, &swarm_size);

    int downloaded_segments = 0;

    for (int i = 0; i < segments_count; i++) {
        char hash[HASH_SIZE + 1];
        request_segment(rank, filename, file_index, segments, hash, i, swarm, swarm_size);

        downloaded_segments++;
        if (downloaded_segments % 10 == 0 &&
            peer_requested[file_index].segments_received_count < segments_count) {
            request_swarm(filename, segments, &segments_count, swarm, &swarm_size);
            peer_status_count = 0;
        }
    }
    // verificam daca toate segmentele au fost descarcate
    if (peer_requested[file_index].segments_received_count == segments_count) {
        for (int i = 0; i < peer_requested[file_index].segments_received_count; i++) {
            create_output_file(rank, filename, peer_requested[file_index].segments[i].hash, i);
        }
        peer_requests_completed++;
    }
}


void *download_thread_func(void *arg)
{
    int rank = *(int*) arg;

    while(peer_requests_completed < peer_requested_count) {
        for(int i = 0; i < peer_requested_count; i++) {
            // proceseaza fiecare fisier solicitat de peer
            process_requested_file_by_peer(rank, peer_requested[i].filename, i);
        }
    }
    // notifica tracker-ul ca peer-ul a terminat descarcarile
    MPI_Send("PEER READY", 20, MPI_CHAR, TRACKER_RANK, 2, MPI_COMM_WORLD);

    return NULL;
}


void *upload_thread_func(void *arg)
{
    int rank = *(int*) arg;

    while (1) {
        MPI_Status status;
        char request_type[20];

        // asteapta cereri de la alti peers
        MPI_Recv(request_type, 20, MPI_CHAR, MPI_ANY_SOURCE, 1, MPI_COMM_WORLD, &status);

        if (strcmp(request_type, "SEGMENT REQUEST") == 0) {
            char filename[MAX_FILENAME];
            int segment_index;
            // primeste numele fisierului si indexul segmentului solicitat
            MPI_Recv(filename, MAX_FILENAME, MPI_CHAR, status.MPI_SOURCE, 1, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            MPI_Recv(&segment_index, 1, MPI_INT, status.MPI_SOURCE, 1, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

            // cauta fisierul solicitat in lista de fisiere ale clientului
            for(int i = 0; i < peer_file_count; i++) {
                if (strcmp(peer_owned_files[i].filename, filename) == 0 &&
                    segment_index >= 0 && segment_index < peer_owned_files[i].segments_count) {

                    MPI_Send(peer_owned_files[i].segments[segment_index].hash, HASH_SIZE, MPI_CHAR, status.MPI_SOURCE, 4, MPI_COMM_WORLD);

                    MPI_Send("ACK", 4, MPI_CHAR, status.MPI_SOURCE, 5, MPI_COMM_WORLD);
                }
            }
        }

        if (strcmp(request_type, "DONE") == 0) {
            return NULL;
        }
    }
}

// tracker-ul primeste informatii despre un fisier de la un peer
void receive_file_data(int source, FileInfo *file) {
    MPI_Recv(file->filename, MAX_FILENAME, MPI_CHAR, source, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

    MPI_Recv(&file->segments_count, 1, MPI_INT, source, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

    file->swarm_size = 0;
    memset(file->swarm, -1, sizeof(file->swarm));

    file->swarm[file->swarm_size++] = source;

    for (int i = 0; i < file->segments_count; i++) {
        segmentInfo segment;
        MPI_Recv(segment.hash, HASH_SIZE, MPI_CHAR, source, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        segment.hash[HASH_SIZE] = '\0';
        segment.index = i;

        file->segments[i] = segment;
    }
}

// tracker-ul colecteaza fisierele detinute de fiecare peer
void add_peer_files_to_tracker(int numtasks, int rank) {
    for (int source = 1; source < numtasks; source++) {
        int num_files_owned;
        MPI_Recv(&num_files_owned, 1, MPI_INT, source, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

        for (int i = 0; i < num_files_owned; i++) {
            FileInfo file;
            receive_file_data(source, &file);

            files[files_count++] = file;
        }
    }
    // notifica toti peers ca informatiile au fost primite
    for (int i = 1; i < numtasks; i++) {
        MPI_Send("ACK", 10, MPI_CHAR, i, 0, MPI_COMM_WORLD);
    }
}


void tracker(int numtasks, int rank) {
    add_peer_files_to_tracker(numtasks, rank);

    int peers_ready[numtasks];
    memset(peers_ready, 0, sizeof(peers_ready));

    while(1) {
        char request[20];
        MPI_Status status;

        // asteapta mesaje de la peers
        MPI_Recv(request, 20, MPI_CHAR, MPI_ANY_SOURCE, 2, MPI_COMM_WORLD, &status);

        if (strcmp(request, "SWARM REQUEST") == 0) {
            char filename_req[MAX_FILENAME];
            MPI_Recv(filename_req, MAX_FILENAME, MPI_CHAR, status.MPI_SOURCE, 2, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

            for(int i = 0; i < files_count; i++) {
                if (strcmp(files[i].filename, filename_req) == 0) {
                    // trimite detalii despre segmente si swarm
                    MPI_Send(&files[i].segments_count, 1, MPI_INT, status.MPI_SOURCE, 3, MPI_COMM_WORLD);
                    for(int j = 0; j < files[i].segments_count; j++) {
                        MPI_Send(files[i].segments[j].hash, HASH_SIZE, MPI_CHAR, status.MPI_SOURCE, 3, MPI_COMM_WORLD);
                        MPI_Send(&files[i].segments[j].index, 1, MPI_INT, status.MPI_SOURCE, 3, MPI_COMM_WORLD);
                    }

                    MPI_Send(&files[i].swarm_size, 1, MPI_INT, status.MPI_SOURCE, 3, MPI_COMM_WORLD);
                    MPI_Send(files[i].swarm, files[i].swarm_size, MPI_INT, status.MPI_SOURCE, 3, MPI_COMM_WORLD);

                    break;
                }
            }
        }

        if (strcmp(request, "PEER READY") == 0) {
            peers_ready[status.MPI_SOURCE] = 1;

            int all_peers_ready = 1;
            for (int i = 1; i < numtasks && all_peers_ready; i++) {
                if (!peers_ready[i]) {
                    all_peers_ready = 0;
                }
            }
            if (all_peers_ready) {
                // notifica toti peers ca procesul este finalizat
                for (int i = 1; i < numtasks; i++) {
                    MPI_Send("DONE", 20, MPI_CHAR, i, 1, MPI_COMM_WORLD);
                }
                return;
            }
        }
    }
}

// citeste fisierele de input
void process_input_file(int rank) {
    char inputfile[MAX_FILENAME];
    sprintf(inputfile, "in%d.txt", rank);

    FILE *file = fopen(inputfile, "r");
    if (!file) {
        exit(-1);
    }

    char hash[HASH_SIZE];

    fscanf(file, "%d", &peer_file_count);

    MPI_Send(&peer_file_count, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD);

    for (int i = 0; i < peer_file_count; i++) {
        fscanf(file, "%s %d", peer_owned_files[i].filename, &peer_owned_files[i].segments_count);
        MPI_Send(peer_owned_files[i].filename, MAX_FILENAME, MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD);
        MPI_Send(&peer_owned_files[i].segments_count, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD);

        for (int j = 0; j < peer_owned_files[i].segments_count; j++) {
            memset(hash, 0, HASH_SIZE + 1);
            fscanf(file, "%32s", hash);
            hash[HASH_SIZE] = '\0';

            strcpy(peer_owned_files[i].segments[j].hash, hash);

            MPI_Send(hash, HASH_SIZE, MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD);
        }
    }
     // memoram fisierele solicitate de clienti
    char filename_req[MAX_FILENAME];
    fscanf(file, "%d", &peer_requested_count);
    for (int i = 0; i < peer_requested_count; i++) {
        fscanf(file, "%s", filename_req);
        strcpy(peer_requested[i].filename, filename_req);
    }

    fclose(file);
}


void peer(int numtasks, int rank) {
    process_input_file(rank);

    pthread_t download_thread;
    pthread_t upload_thread;
    void *status;
    int r;

    r = pthread_create(&download_thread, NULL, download_thread_func, (void *) &rank);
    if (r) {
        printf("Eroare la crearea thread-ului de download\n");
        exit(-1);
    }

    r = pthread_create(&upload_thread, NULL, upload_thread_func, (void *) &rank);
    if (r) {
        printf("Eroare la crearea thread-ului de upload\n");
        exit(-1);
    }

    r = pthread_join(download_thread, &status);
    if (r) {
        printf("Eroare la asteptarea thread-ului de download\n");
        exit(-1);
    }

    r = pthread_join(upload_thread, &status);
    if (r) {
        printf("Eroare la asteptarea thread-ului de upload\n");
        exit(-1);
    }
}

 
int main (int argc, char *argv[]) {
    int numtasks, rank;
 
    int provided;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
    if (provided < MPI_THREAD_MULTIPLE) {
        fprintf(stderr, "MPI nu are suport pentru multi-threading\n");
        exit(-1);
    }
    MPI_Comm_size(MPI_COMM_WORLD, &numtasks);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    if (rank == TRACKER_RANK) {
        tracker(numtasks, rank);
    } else {
        peer(numtasks, rank);
    }

    MPI_Finalize();
}