/*
Created by Giorgio Caggegi on 2025/05/30

Questo programma copia file regolari, symlinks e directory (ricorsivamente)
all'interno della directory passata come ultimo argomento.
Vengono copiati anche i timestamp di creazione e ultimo accesso.
I file copiati avranno gli stessi permessi di quelli originali.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <libgen.h>
#include <stdbool.h>
#include <sys/stat.h>

#define MAXPATH 4096 // Valore comune
#define EXITMSG(...)                                                           \
    do {                                                                       \
        fprintf(stderr, __VA_ARGS__);                                          \
        fprintf(stderr, " : ");                                                \
        perror("");                                                            \
        exit(EXIT_FAILURE);                                                    \
    } while (0)

#define USAGE "Usage: %s <reg file> <sym link> <dir> ... <dest dir>"

// Forward declaration
void copy_discern(char *src, char *dst);

// Ritorna una stringa allocata dinamicamente che è
// la concatenazione di dst_dir, percorso di una cartella, con
// il basename estratto da src, separate da uno slash
char *get_compath(char *src, char *dst_dir) {

    char *src_dup, *tofree, *toret, *filename;
    size_t dd_len = strlen(dst_dir);
    size_t src_len = strlen(src);
    size_t toret_sz = dd_len+src_len+2;

    // Duplicazione di src
    if (!(src_dup = strdup(src))) {
        EXITMSG("get_compath(): strdup() error on %s", src);
    }
    tofree = src_dup;

    // Il nome del file estratto dalla copia di src
    filename = basename(src_dup);

    // Uno in più per lo slash, uno in più per il null char
    toret = malloc(toret_sz * sizeof(char));
    toret[toret_sz - 1] = '\0';

    // Composizione del percorso
    sprintf(toret, "%s/%s", dst_dir, filename);

    free(tofree);
    return toret;
}

// Nelle prossime funzioni, dst (o dd_path) deve essere un percorso valido di una
// cartella con accesso in lettura e scrittura
// e src_stat deve puntare alle stat di src

// src deve essere un path valido di un file regolare
void copy_file(char *src, char *dst, struct stat *src_stat) {

    int ds, dd, byte_read;
    char buffer[BUFSIZ];

    // Ottengo il percorso completo del file di destinazione
    char *complete_path = get_compath(src, dst);

    // Apro il file sorgente
    if ((ds = open(src, O_RDONLY)) < 0) {
        EXITMSG("copy_file(): open() error on %s", src);
    }

    // Apro il file destinazione
    if ((dd = open(complete_path, O_WRONLY | O_TRUNC | O_CREAT, src_stat->st_mode)) < 0) {
        EXITMSG("copy_file(): open() error on %s", complete_path);
    }
    
    // Scrivo il file destinazione
    do {
        if ((byte_read = read(ds, buffer, BUFSIZ)) == -1) {
            EXITMSG("copy_file(): read() error on %s", src);
        }

        if ((write(dd, buffer, byte_read)) == -1) {
            EXITMSG("copy_file(): write() error on %s", complete_path);
        }
    } while (BUFSIZ == byte_read);

    // Copio i timestamp
    struct timespec times[2] = {src_stat->st_atim, src_stat->st_mtim};
    if (futimens(dd, times) == -1) {
        fprintf(stderr, "copy_file(): futimens() error on %s : ", complete_path);
        perror("");
        // Il processo non esce in caso di errore
    };

    // Chiudo i file aperti
    if (close(ds) == -1) {
        fprintf(stderr, "copy_file(): close() error on %s : ", src);
        perror("");
        // Il processo non esce in caso di errore
    };
    if (close(dd) == -1) {
        fprintf(stderr, "copy_file(): close() error on %s : ", complete_path);
        perror("");
        // Il processo non esce in caso di errore
    };

    free(complete_path);
}

// src deve essere un path valido di un symlink
void copy_slink(char *src, char *dst, struct stat *src_stat) {
    
    char buffer[MAXPATH];
    ssize_t copiati;

    if ((copiati = readlink(src, buffer, MAXPATH)) == MAXPATH) { // Buffer troppo corto
        EXITMSG("copy_slink(): symlink path too long");
    }
    
    // Lettura del percorso puntato dal symlink riuscita
    // Inserisco il null char
    buffer[copiati] = '\0';
    
    // Costruisco il percorso completo del file symlink di destinazione
    char *dst_path = get_compath(src, dst);

    // Creo il symlink
    if (symlink(buffer, dst_path) == -1) {
        if (errno = EEXIST) {
            fprintf(stderr, "copy_slink(): ignored copy of %s : ", src);
            perror("");
            free(dst_path);
            return;
            // Il processo non esce in caso di file già esistente
        } else {
            EXITMSG("copy_slink(): symlink() error on %s", buffer);
        }
    }

    // Copio i timestamp
    // Il processo non esce in caso di errore
    struct stat napoli;
    if (lstat(src, &napoli) != 0) {
        fprintf(stderr, "Error on gathering lstat on %s : ", src);
        perror("");
    } else {
        struct timespec times[2] = { {napoli.st_atime}, {napoli.st_mtime}};
        if (utimensat(AT_FDCWD, dst_path, times, AT_SYMLINK_NOFOLLOW) == -1) {
            fprintf(stderr, "Error on setting metadata on %s : ", src);
            perror("");
        }
    }

    free(dst_path);
}

// sd_path deve essere un path valido di una directory con adeguati permessi 
// (lettura e attraversamento)
// sd: source directory; dd: destination directory
void copy_dir(char *sd_path, char *dd_1path, struct stat *src_stat) {

    DIR *sd_stream;
    struct dirent *entry;

    // Devo creare la directory di destinazione con lo stesso nome originale
    // e gli stessi permessi, dentro la directory con percorso dd_1path
    // dd_compath sarà dd_1path/sd_path
    char *dd_compath = get_compath(sd_path, dd_1path);
    if (mkdir(dd_compath, src_stat->st_mode) == -1) {
        if (errno == EEXIST) {
            fprintf(stderr, "copy_dir(): ignored copy of %s : ", sd_path);
            perror("");
        } else {
            EXITMSG("copy_dir(): mkdir() error of %s", dd_compath);
        }
    }

    // Ottengo i timestamps della cartella che sto esplorando
    struct timespec times[2] = {src_stat->st_atim, src_stat->st_mtim};

    // Imposto i timestamps della cartella appena creata
    if (utimensat(AT_FDCWD, dd_compath, times, 0) == -1) {
        fprintf(stderr, "copy_dir(): utimensat() error on %s\n", dd_compath);
        perror("");
        // Il processo non esce in caso di errore
    }
    
    // Creo il directory stream
    if (!(sd_stream = opendir(sd_path))) {
        EXITMSG("copy_dir(): opendir() error on %s", sd_path);
    }
    
    while ((entry = readdir(sd_stream))) {

        // Ignoro le directories virtuali
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, ".." ) == 0) {
            continue;
        }

        // Concateno il nome dell'entry con il percorso della cartella
        size_t sf_compath_sz = strlen(sd_path) + strlen(entry->d_name) + 2;
        char sf_compath[sf_compath_sz];
        sf_compath[sf_compath_sz-1] = '\0';
        sprintf(sf_compath, "%s/%s", sd_path, entry->d_name);

        // Il nuovo file sarà a dd_compath/basename(sf_compath)
        copy_discern(sf_compath, dd_compath);
    }

    free(dd_compath);
}

// Manda un messaggio sullo stderr se src non è percorso di un file regolare,
// di un symlink o di una directory
// In caso, lo ignora
void copy_discern(char *src, char *dst) {
    struct stat napoli;

    if (lstat(src, &napoli) == -1) {
        EXITMSG("Error on gathering stat on %s", src);
    }

    if (S_ISLNK(napoli.st_mode))
        copy_slink(src, dst, &napoli);

    else if (S_ISREG(napoli.st_mode))
        copy_file(src, dst, &napoli);

    else if (S_ISDIR(napoli.st_mode))
        copy_dir(src, dst, &napoli);

    else
        fprintf(stderr, "%s : Wrong file type\n", src);
}

void check_args_iterate_paths(int argc, char **argv) {

    if (argc < 3) {
        EXITMSG(USAGE, argv[0]);
    }

    // Controllo l'ultimo argomento
    // Deve essere una cartella per la quale sono granted i necessari permessi
    if (access(argv[argc-1], X_OK | W_OK) == -1) {
        EXITMSG("access() error on destination folder");
    }
    
    // Itero tutti i percorsi da copiare
    for (int i = 1; i <= argc-2; i++)
        copy_discern(argv[i], argv[argc-1]);
}


int main(int argc, char **argv) {

    check_args_iterate_paths(argc, argv);

    exit(EXIT_SUCCESS);
}