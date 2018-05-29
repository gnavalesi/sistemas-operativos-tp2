#include "node.h"
#include "picosha2.h"
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <cstdlib>
#include <queue>
#include <atomic>
#include <mpi.h>
#include <map>
#include <mutex>
#include <iostream>

int total_nodes, mpi_rank;
Block *last_block_in_chain;
map<string, Block> node_blocks;
mutex receive_mutex, last_block_mutex;
atomic<bool> finished_mining;

void run(int result, const string &message) {
    if (result != MPI_SUCCESS) {
        cerr << message << endl;
        exit(-1);
    }
}

//Cuando me llega una cadena adelantada, y tengo que pedir los nodos que me faltan
//Si nos separan más de VALIDATION_BLOCKS bloques de distancia entre las cadenas, se descarta por seguridad
bool verificar_y_migrar_cadena(const Block *rBlock, const MPI_Status *status) {

    //FIXME: Enviar mensaje TAG_CHAIN_HASH
    auto *blockchain = new Block[VALIDATION_BLOCKS];
    MPI_Status response_status{};
    int i;


    printf("[%d] Pidiendo cadena a %d empezando por el hash %s\n", mpi_rank, status->MPI_SOURCE, rBlock->block_hash);
    run(MPI_Send(rBlock->block_hash, HASH_SIZE, MPI_CHAR, status->MPI_SOURCE, TAG_CHAIN_HASH, MPI_COMM_WORLD),
        "Error: unable to send TAG_CHAIN_HASH message");

    //FIXME: Recibir mensaje TAG_CHAIN_RESPONSE
    printf("[%d] Recibiendo bloques de %d\n", mpi_rank, status->MPI_SOURCE);
    run(MPI_Recv(blockchain, VALIDATION_BLOCKS, *MPI_BLOCK, status->MPI_SOURCE, TAG_CHAIN_RESPONSE, MPI_COMM_WORLD,
                 &response_status),
        "Error: unable to receive TAG_CHAIN_RESPONSE message");
    printf("[%d] Bloques recibidos de %d\n", mpi_rank, status->MPI_SOURCE);

    //FIXME: Verificar que los bloques recibidos
    //sean válidos y se puedan acoplar a la cadena

    if (string(blockchain[0].block_hash) == string(rBlock->block_hash) &&
        blockchain[0].index == rBlock->index) {

        printf("[%d] Primer bloque enviado por %d es valido\n", mpi_rank, status->MPI_SOURCE);

        if (blockchain[0].block_hash == block_to_str(&blockchain[0])) {
            bool valid = true;
            bool found = false;

            for (i = 0; i < VALIDATION_BLOCKS - 1; i++) {
                if (blockchain[i].index > 1) {
                    valid = valid && string(blockchain[i].previous_block_hash) == string(blockchain[i + 1].block_hash)
                            && blockchain[i].index == blockchain[i + 1].index + 1;
                    found = node_blocks.count(blockchain[i].block_hash) == 1;
                } else {
                    found = found || blockchain[i].index == 1;
                    break;
                }
            }

            if (valid && found) {
                *last_block_in_chain = blockchain[0];
                for (i = 0; i < VALIDATION_BLOCKS; i++) {
                    if (node_blocks.count(blockchain[i].block_hash) == 0) {
                        node_blocks[string(blockchain[i].block_hash)] = blockchain[i];

                        if (blockchain[i].index == 1) {
                            break;
                        }
                    } else {
                        break;
                    }
                }

                delete[]blockchain;
                return true;
            }
        }
    }

    delete[]blockchain;
    return false;
}

//Verifica que el bloque tenga que ser incluido en la cadena, y lo agrega si corresponde
bool validate_block_for_chain(const Block *rBlock, const MPI_Status *status) {
    if (valid_new_block(rBlock)) {

        bool ret;

        //Agrego el bloque al diccionario, aunque no
        //necesariamente eso lo agrega a la cadena
        node_blocks[string(rBlock->block_hash)] = *rBlock;

        // Lockeo last_block_in_chain porque lo voy a consultar y posiblemente modificar
        last_block_mutex.lock();

        //FIXME: Si el índice del bloque recibido es 1
        //y mí último bloque actual tiene índice 0,
        //entonces lo agrego como nuevo último.
        if (rBlock->index == 1 && last_block_in_chain->index == 0) {
            *last_block_in_chain = *rBlock;
            ret = true;
            printf("[%d] Agregado a la lista bloque con index %d enviado por %d \n", mpi_rank,
                   rBlock->index, status->MPI_SOURCE);
        }

        //FIXME: Si el índice del bloque recibido es
        //el siguiente a mí último bloque actual...
        else if (last_block_in_chain->index + 1 == rBlock->index) {
            //...y el bloque anterior apuntado por el recibido es mí último actual,
            //entonces lo agrego como nuevo último.
            if (string(rBlock->previous_block_hash) == string(last_block_in_chain->block_hash)) {
                *last_block_in_chain = *rBlock;
                ret = true;
                printf("[%d] Agregado a la lista bloque con index %d enviado por %d \n", mpi_rank,
                       rBlock->index, status->MPI_SOURCE);
            }
            //...pero el bloque anterior apuntado por el recibido no es mí último actual,
            //entonces hay una blockchain más larga que la mía.
            else {
                ret = verificar_y_migrar_cadena(rBlock, status);
                printf("[%d] Perdí la carrera por uno (%d) contra %d \n", mpi_rank, rBlock->index,
                       status->MPI_SOURCE);
            }
        }

        //FIXME: Si el índice del bloque recibido es igua al índice de mi último bloque actual,
        //entonces hay dos posibles forks de la blockchain pero mantengo la mía
        else if (rBlock->index == last_block_in_chain->index) {
            ret = false;
            printf("[%d] Conflicto suave: Conflicto de branch (%d) contra %d \n", mpi_rank, rBlock->index,
                   status->MPI_SOURCE);
        }

        //FIXME: Si el índice del bloque recibido es anterior al índice de mi último bloque actual,
        //entonces lo descarto porque asumo que mi cadena es la que está quedando preservada.
        else if (rBlock->index < last_block_in_chain->index) {
            ret = false;
            printf("[%d] Conflicto suave: Descarto el bloque (%d vs %d) contra %d \n", mpi_rank, rBlock->index,
                   last_block_in_chain->index, status->MPI_SOURCE);
        }

        //FIXME: Si el índice del bloque recibido está más de una posición adelantada a mi último bloque actual,
        //entonces me conviene abandonar mi blockchain actual
        else if (rBlock->index > last_block_in_chain->index + 1) {
            ret = verificar_y_migrar_cadena(rBlock, status);
            printf("[%d] Perdí la carrera por varios contra %d \n", mpi_rank, status->MPI_SOURCE);
        }

        last_block_mutex.unlock();

        return ret;
    }

    printf("[%d] Error duro: Descarto el bloque recibido de %d porque no es válido \n", mpi_rank, status->MPI_SOURCE);
    return false;
}


//Envia el bloque minado a todos los nodos
void broadcast_block(const Block *block) {
    //No enviar a mí mismo
    //FIXME: Completar

    int r = mpi_rank, i = 0;
    MPI_Request requests[total_nodes-1];

    while ((r = (r+1) % total_nodes) != mpi_rank) {
        run(MPI_Isend(block, 1, *MPI_BLOCK, r, TAG_NEW_BLOCK, MPI_COMM_WORLD, &requests[i++]),
            "Error: unable to send TAG_NEW_BLOCK message");
    }

    run(MPI_Waitall(total_nodes-1, requests, MPI_STATUSES_IGNORE),
        "Error: unable to wait for TAG_NEW_BLOCK message completion");
}

//Proof of work
//FIXME: Advertencia: puede tener condiciones de carrera
void *proof_of_work(void *ptr) {
    string hash_hex_str;
    Block block;
    unsigned int mined_blocks = 0;
    while (mined_blocks < BLOCKS_TO_MINE) {

        last_block_mutex.lock();
        block = *last_block_in_chain;
        last_block_mutex.unlock();

        //Preparar nuevo bloque
        block.index += 1;
        block.node_owner_number = mpi_rank;
        block.difficulty = DEFAULT_DIFFICULTY;
        memcpy(block.previous_block_hash, block.block_hash, HASH_SIZE);

        //Agregar un nonce al azar al bloque para intentar resolver el problema
        gen_random_nonce(block.nonce);

        //Hashear el contenido (con el nuevo nonce)
        block_to_hash(&block, hash_hex_str);

        //Contar la cantidad de ceros iniciales (con el nuevo nonce)
        if (solves_problem(hash_hex_str)) {

            //Verifico que no haya cambiado mientras calculaba
            last_block_mutex.lock();
            if (last_block_in_chain->index < block.index) {
                mined_blocks += 1;
                *last_block_in_chain = block;
                strcpy(last_block_in_chain->block_hash, hash_hex_str.c_str());
                last_block_in_chain->created_at = static_cast<unsigned long int> (time(NULL));
                node_blocks[hash_hex_str] = *last_block_in_chain;
                printf("[%d] Agregué un bloque producido con index %d \n", mpi_rank, last_block_in_chain->index);

                //FIXME: Mientras comunico, no responder mensajes de nuevos nodos
                receive_mutex.lock();
                broadcast_block(last_block_in_chain);
                receive_mutex.unlock();
            }
            last_block_mutex.unlock();
        }

    }

    finished_mining.store(true);

    printf("[%d] Termine de minar \n", mpi_rank);

    return NULL;
}


int node() {
    pthread_t miner_thread;

    MPI_Status message_status{};
    auto new_block = new Block;
    char block_hash[HASH_SIZE];

    Block *blocks_to_send = new Block[VALIDATION_BLOCKS];
    int i;

    //Tomar valor de mpi_rank y de nodos totales
    MPI_Comm_size(MPI_COMM_WORLD, &total_nodes);
    MPI_Comm_rank(MPI_COMM_WORLD, &mpi_rank);

    //La semilla de las funciones aleatorias depende del mpi_ranking
    srand(time(NULL) + mpi_rank);
    printf("[MPI] Lanzando proceso %u\n", mpi_rank);

    last_block_in_chain = new Block;

    //Inicializo el primer bloque
    last_block_in_chain->index = 0;
    last_block_in_chain->node_owner_number = mpi_rank;
    last_block_in_chain->difficulty = DEFAULT_DIFFICULTY;
    last_block_in_chain->created_at = static_cast<unsigned long int> (time(NULL));
    memset(last_block_in_chain->previous_block_hash, 0, HASH_SIZE);

    finished_mining.store(false);

    //FIXME: Crear thread para minar
    if (pthread_create(&miner_thread, nullptr, proof_of_work, nullptr)) {
        cerr << "Error: unable to create thread" << endl;
        exit(-1);
    }

    while (!finished_mining.load()) {
        //FIXME: Recibir mensajes de otros nodos

        run(MPI_Probe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &message_status),
            "Error: unable to probe message");

        if (message_status.MPI_TAG == TAG_NEW_BLOCK) {
            //FIXME: Si es un mensaje de nuevo bloque, llamar a la función
            // validate_block_for_chain con el bloque recibido y el estado de MPI

            run(MPI_Recv(new_block, 1, *MPI_BLOCK, message_status.MPI_SOURCE, TAG_NEW_BLOCK,
                MPI_COMM_WORLD, &message_status), "Error: unable to receive TAG_NEW_BLOCK message");

            receive_mutex.lock();
            validate_block_for_chain(new_block, &message_status);
            receive_mutex.unlock();

        } else if (message_status.MPI_TAG == TAG_CHAIN_HASH) {
            //FIXME: Si es un mensaje de pedido de cadena,
            //responderlo enviando los bloques correspondientes

            run(MPI_Recv(block_hash, HASH_SIZE, MPI_CHAR, message_status.MPI_SOURCE, TAG_CHAIN_HASH,
                MPI_COMM_WORLD, &message_status), "Error: unable to receive TAG_CHAIN_HASH message");

            string hash = string(block_hash);
            i = 0;
            do {
                blocks_to_send[i] = node_blocks.at(hash);
                hash = string(blocks_to_send[i].block_hash);
            }
            while (blocks_to_send[i].index != 1 && ++i < VALIDATION_BLOCKS);

            run(MPI_Send(blocks_to_send, VALIDATION_BLOCKS, *MPI_BLOCK, message_status.MPI_SOURCE,
                TAG_CHAIN_RESPONSE, MPI_COMM_WORLD), "Error: unable to send TAG_CHAIN_RESPONSE message");
        }
    }

    delete new_block;
    delete last_block_in_chain;
    delete[] blocks_to_send;

    return 0;
}