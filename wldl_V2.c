/*
 * ytdl_wl.c — YouTube Watch Later Downloader (réécriture en C du script bash)
 * ---------------------------------------------------------------------------
 * Reprend le comportement du script bash d'origine (serveur POT, analyse de
 * la playlist "Watch Later", téléchargement via yt-dlp) avec :
 *
 *   1. Une sortie terminal "curatée" : seules les infos utiles (titres en
 *      cours, erreurs, avertissements, résumé final) sont affichées.
 *
 *   2. DEUX fichiers de logs par run, dans .../999. tools/logs/<AAAAMMJJ_HHMMSS>/ :
 *
 *        <ts>_log_detail.txt  -> RAW. Tout : sortie verbeuse/traffic de
 *          yt-dlp (y compris cookies), plus les décisions internes du
 *          programme ([prog] ...). Volontairement PAS pensé pour être lu
 *          par un humain : c'est le fichier à donner à une IA (ou à
 *          relire soi-même) pour déboguer un run qui a mal tourné.
 *          -> Contient potentiellement des cookies de session en clair
 *             (issus de --print-traffic). Le fichier est créé en 0600
 *             (lecture/écriture propriétaire uniquement), et le dossier
 *             du run en 0700, pour limiter l'exposition.
 *
 *        <ts>_log_clear.txt   -> CURATÉ. Construit explicitement par le
 *          programme (pas par filtrage de la sortie yt-dlp) : déroulé
 *          chronologique lisible par un humain, avec davantage de détail
 *          que le terminal (format retenu par vidéo, décisions prises
 *          sur la qualité, avertissements, erreurs, résumé final).
 *
 *        <ts>_pot_server.txt  -> sortie brute du serveur POT (node), dans
 *          son propre fichier pour ne pas s'entremêler avec le reste
 *          (deux process écrivant dans le même fd en parallèle produirait
 *          un fichier détail illisible et non déterministe).
 *
 *   3. Purge automatique des dossiers de logs, avec DEUX seuils distincts
 *      selon le statut du run (encodé dans le suffixe du nom de dossier,
 *      "_OK" ou "_ERR", ajouté au renommage final du dossier) :
 *        - run SANS erreur : supprimé après LOG_MAX_AGE_DAYS_OK jours
 *        - run AVEC au moins une erreur : supprimé après LOG_MAX_AGE_DAYS_ERR jours
 *      L'âge est calculé à partir de l'horodatage contenu dans le NOM du
 *      dossier (format fiable et portable), pas des métadonnées fichier
 *      (birthtime/mtime), qui peuvent être altérées par une sauvegarde/sync.
 *      Un dossier dont le nom ne peut pas être daté (format inconnu) n'est
 *      jamais touché. Un dossier sans suffixe _OK/_ERR reconnu (run
 *      interrompu avant la fin) est traité comme "_ERR" par prudence.
 *
 *   4. Politique qualité vidéo : le serveur POT fournit les "PO Tokens"
 *      nécessaires à yt-dlp pour débloquer les formats haute qualité sur
 *      YouTube. S'il ne répond pas dans les POT_WAIT_MAX_SECONDS secondes
 *      impartis, le programme ne télécharge PAS silencieusement en moins
 *      bonne qualité : il bascule en "mode strict", qui exige un format
 *      d'au moins MIN_HEIGHT_STRICT pixels de hauteur. Si un format
 *      satisfaisant cette contrainte n'existe pas pour une vidéo donnée,
 *      yt-dlp remonte une erreur ; dans ce cas précis le programme arrête
 *      PROPREMENT l'ensemble du run (le process yt-dlp est terminé via
 *      SIGTERM, les logs sont finalisés, le code retour est non nul)
 *      plutôt que de continuer à télécharger le reste de la playlist ou
 *      d'accepter une qualité dégradée.
 *
 *   NB (fragilité assumée) : comme dans la version précédente, plusieurs
 *   heuristiques de ce fichier (maybe_capture_format, la détection des
 *   lignes "[download] 100%", "has already been recorded", etc.) reposent
 *   sur le format textuel ACTUEL des messages de yt-dlp. Si une future
 *   version de yt-dlp change ce format, seul l'enrichissement du log clear
 *   et/ou la bascule qualité stricte peuvent en pâtir (dégradation
 *   silencieuse de l'info affichée), pas la correction du téléchargement
 *   lui-même qui reste piloté par le sélecteur de format donné à yt-dlp.
 *
 * Compilation (macOS/Linux) :
 *   clang -O2 -Wall -Wextra -o ytdl_wl ytdl_wl.c
 *
 * Dépendances externes (doivent être dans le PATH) : yt-dlp, node, curl.
 * ---------------------------------------------------------------------------
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <ctype.h>
#include "config.local.h"

#define FORMAT_SELECTOR_NORMAL "bv*+ba/b"
#define FORMAT_SELECTOR_STRICT "bv*[height>=1080]+ba/b[height>=1080]"

/* Marqueurs internes utilisés pour piloter l'affichage / la collecte de
 * données. Ils apparaissent tels quels dans le log detail (raw), mais sont
 * traduits en lignes lisibles dans le log clear. */
#define MARK_ID    "@@ID@@:"
#define MARK_TITLE "@@TITLE@@:"

/* ===========================================================================
 * COULEURS TERMINAL (désactivées si la sortie n'est pas un terminal)
 * ======================================================================== */
static const char *COL_RESET = "";
static const char *COL_BOLD  = "";
static const char *COL_DIM   = "";
static const char *COL_RED   = "";
static const char *COL_GREEN = "";
static const char *COL_YELLOW= "";
static const char *COL_CYAN  = "";
static const char *COL_BLUE  = "";

static void init_colors(int enable) {
    if (!enable) return;
    COL_RESET  = "\033[0m";
    COL_BOLD   = "\033[1m";
    COL_DIM    = "\033[2m";
    COL_RED    = "\033[31m";
    COL_GREEN  = "\033[32m";
    COL_YELLOW = "\033[33m";
    COL_CYAN   = "\033[36m";
    COL_BLUE   = "\033[34m";
}

/* ===========================================================================
 * TABLEAU DYNAMIQUE DE CHAINES (utilisé pour les IDs, erreurs, etc.)
 * ======================================================================== */
typedef struct {
    char **items;
    size_t count;
    size_t cap;
} StrArray;

static void sa_init(StrArray *a) { a->items = NULL; a->count = 0; a->cap = 0; }

static void sa_push(StrArray *a, const char *s) {
    if (a->count == a->cap) {
        size_t new_cap = a->cap ? a->cap * 2 : 64;
        char **tmp = realloc(a->items, new_cap * sizeof(char *));
        if (!tmp) {
            fprintf(stderr, "Erreur mémoire critique (realloc a échoué) -- arrêt.\n");
            abort();
        }
        a->items = tmp;
        a->cap = new_cap;
    }
    a->items[a->count++] = strdup(s);
}

static void sa_free(StrArray *a) {
    for (size_t i = 0; i < a->count; i++) free(a->items[i]);
    free(a->items);
    a->items = NULL;
    a->count = a->cap = 0;
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char **)a, *(const char **)b);
}

static void sa_sort_unique(StrArray *a) {
    if (a->count == 0) return;
    qsort(a->items, a->count, sizeof(char *), cmp_str);
    size_t w = 1;
    for (size_t i = 1; i < a->count; i++) {
        if (strcmp(a->items[i], a->items[w - 1]) != 0) {
            a->items[w++] = a->items[i];
        } else {
            free(a->items[i]);
        }
    }
    a->count = w;
}

static int sa_bsearch(const StrArray *sorted, const char *key) {
    int lo = 0, hi = (int)sorted->count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = strcmp(sorted->items[mid], key);
        if (c == 0) return 1;
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

/* ===========================================================================
 * UTILITAIRES TEXTE
 * ======================================================================== */

/* Retire les séquences d'échappement ANSI (couleurs) éventuelles, in place. */
static void strip_ansi(char *s) {
    char *src = s, *dst = s;
    while (*src) {
        if (*src == '\033' && src[1] == '[') {
            src += 2;
            while (*src && !((*src >= 'A' && *src <= 'Z') || (*src >= 'a' && *src <= 'z'))) src++;
            if (*src) src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

/* Un ID vidéo YouTube = exactement 11 caractères alphanum/_/- */
static int extract_id(const char *s, char out11[12]) {
    for (int i = 0; i < 11; i++) {
        char c = s[i];
        if (c == '\0') return 0;
        if (!(isalnum((unsigned char)c) || c == '_' || c == '-')) return 0;
    }
    char boundary = s[11];
    if (boundary != '\0' && isalnum((unsigned char)boundary)) return 0;
    memcpy(out11, s, 11);
    out11[11] = '\0';
    return 1;
}

static char *now_timestamp(void) {
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char *buf = malloc(20);
    strftime(buf, 20, "%Y%m%d_%H%M%S", &tmv);
    return buf;
}

/* ===========================================================================
 * SYSTEME DE FICHIERS : mkdir -p / suppression récursive / purge des logs
 * ======================================================================== */
static int mkdir_p(const char *path) {
    char tmp[2048];
    snprintf(tmp, sizeof tmp, "%s", path);
    size_t len = strlen(tmp);
    if (len > 0 && tmp[len - 1] == '/') tmp[len - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int remove_dir_recursive(const char *path) {
    DIR *d = opendir(path);
    if (!d) return -1;
    struct dirent *ent;
    char child[2048];
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        snprintf(child, sizeof child, "%s/%s", path, ent->d_name);
        struct stat st;
        if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
            remove_dir_recursive(child);
        } else {
            unlink(child);
        }
    }
    closedir(d);
    return rmdir(path);
}

/* Parse l'horodatage encodé dans le nom d'un dossier de logs, au format
 * "AAAAMMJJ_HHMMSS" (éventuellement suivi de "_OK" ou "_ERR"). Renvoie 0 si
 * le nom ne correspond pas à ce format (dossier non touché par la purge). */
static int parse_run_timestamp(const char *dirname, time_t *out) {
    if (strlen(dirname) < 15) return 0;
    char buf[16];
    memcpy(buf, dirname, 15);
    buf[15] = '\0';
    struct tm tmv;
    memset(&tmv, 0, sizeof tmv);
    if (strptime(buf, "%Y%m%d_%H%M%S", &tmv) == NULL) return 0;
    tmv.tm_isdst = -1;
    time_t t = mktime(&tmv);
    if (t == (time_t)-1) return 0;
    *out = t;
    return 1;
}

typedef enum { RUN_STATUS_OK, RUN_STATUS_ERR, RUN_STATUS_UNKNOWN } RunStatus;

static RunStatus dirname_status(const char *name) {
    size_t len = strlen(name);
    if (len >= 4 && strcmp(name + len - 4, "_ERR") == 0) return RUN_STATUS_ERR;
    if (len >= 3 && strcmp(name + len - 3, "_OK") == 0) return RUN_STATUS_OK;
    return RUN_STATUS_UNKNOWN;
}

static void cleanup_old_logs(void) {
    DIR *d = opendir(LOGS_DIR);
    if (!d) return; /* pas encore de dossier logs : rien à purger */
		struct dirent *ent;
		time_t now = time(NULL);
		int removed = 0;
		char path[2048];
		while ((ent = readdir(d)) != NULL) {
			if (ent->d_name[0] == '.') continue;
			snprintf(path, sizeof path, "%s/%s", LOGS_DIR, ent->d_name);
			struct stat st;
			if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

			time_t run_time;
			if (!parse_run_timestamp(ent->d_name, &run_time)) {
				/* Nom non reconnu : par prudence, on ne touche jamais à un
				   dossier qu'on ne sait pas dater avec certitude. */
				continue;
			}

			double age_days = difftime(now, run_time) / 86400.0;
			RunStatus st_run = dirname_status(ent->d_name);
			/* UNKNOWN (run interrompu avant renommage final) traité comme ERR :
			   on préfère garder trop longtemps plutôt que supprimer trop tôt. */
			int max_age = (st_run == RUN_STATUS_OK) ? LOG_MAX_AGE_DAYS_OK : LOG_MAX_AGE_DAYS_ERR;

			if (age_days > max_age) {
				if (remove_dir_recursive(path) == 0) removed++;
			}
		}
		closedir(d);
		if (removed > 0) {
			printf("%s🧹 %d ancien(s) dossier(s) de logs supprimé(s) (règle : %dj sans erreur / %dj avec erreur)%s\n",
				   COL_DIM, removed, LOG_MAX_AGE_DAYS_OK, LOG_MAX_AGE_DAYS_ERR, COL_RESET);
		}
	}

	/* ===========================================================================
	 * ARCHIVE downloaded.txt : combien d'IDs de la Watch Later sont déjà dedans
	 * ======================================================================== */
	static int count_archived(const StrArray *wl_ids) {
		FILE *f = fopen(ARCHIVE_FILE, "r");
		if (!f) return 0;

		StrArray arc;
		sa_init(&arc);

		char *line = NULL;
		size_t cap = 0;
		ssize_t len;
		while ((len = getline(&line, &cap, f)) != -1) {
			while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';

			char *copy = strdup(line);
			char *last = NULL;
			char *tok = strtok(copy, " \t");
			while (tok) { last = tok; tok = strtok(NULL, " \t"); }

			if (last && strlen(last) == 11) {
				char id[12];
				if (extract_id(last, id)) sa_push(&arc, id);
			}
			free(copy);
		}
		free(line);
		fclose(f);

		sa_sort_unique(&arc);

		int count = 0;
		for (size_t i = 0; i < wl_ids->count; i++) {
			if (sa_bsearch(&arc, wl_ids->items[i])) count++;
		}
		sa_free(&arc);
		return count;
	}

	/* ===========================================================================
	 * LOGS : les deux fichiers du run (voir commentaire d'en-tête)
	 * ======================================================================== */
	typedef struct {
		FILE *detail; /* RAW : tout, pensé pour être relu par une IA en cas de souci */
		FILE *clear;  /* CURATÉ : lisible par un humain en un coup d'oeil */
	} Logs;

	/* ===========================================================================
	 * AFFICHAGE CURATÉ (terminal + log clear)
	 * ======================================================================== */
	static void print_banner(Logs *logs) {
		printf("%s%s======================================%s\n", COL_BOLD, COL_BLUE, COL_RESET);
		printf("%s  YouTube Watch Later Downloader%s\n", COL_BOLD, COL_RESET);
		printf("%s%s======================================%s\n\n", COL_BOLD, COL_BLUE, COL_RESET);

		fprintf(logs->clear, "======================================\n");
		fprintf(logs->clear, "  YouTube Watch Later Downloader\n");
		fprintf(logs->clear, "======================================\n\n");
	}

	static void print_section(Logs *logs, const char *title) {
		printf("\n%s▶ %s%s\n", COL_BOLD, title, COL_RESET);
		fprintf(logs->clear, "\n▶ %s\n", title);
	}

	static void print_summary(Logs *logs, int status, int err_count, int warn_count, int skip_count,
							   const StrArray *err_lines, const StrArray *err_ids,
							   long secs, int aborted_for_quality) {
		printf("\n%s======================================%s\n", COL_BOLD, COL_RESET);
		fprintf(logs->clear, "\n======================================\n");

		if (aborted_for_quality) {
			printf("%s⛔ Arrêt anticipé : qualité >= %dp non garantie (serveur POT indisponible)%s\n",
				   COL_RED, MIN_HEIGHT_STRICT, COL_RESET);
			fprintf(logs->clear, "⛔ Arrêt anticipé : qualité >= %dp non garantie (serveur POT indisponible)\n",
					MIN_HEIGHT_STRICT);
		} else if (status == 0 && err_count == 0) {
			printf("%s✅ Terminé sans erreur%s\n", COL_GREEN, COL_RESET);
			fprintf(logs->clear, "✅ Terminé sans erreur\n");
		} else {
			printf("%s⚠️  Terminé avec %d erreur(s)%s\n", COL_YELLOW, err_count, COL_RESET);
			fprintf(logs->clear, "⚠️  Terminé avec %d erreur(s)\n", err_count);
		}

		if (err_count > 0) {
			printf("\n");
			for (size_t i = 0; i < err_lines->count; i++) {
				printf("  %s•%s %s\n", COL_RED, COL_RESET, err_lines->items[i]);
					printf("    ID : %s\n\n", err_ids->items[i]);
					fprintf(logs->clear, "  • %s (ID : %s)\n", err_lines->items[i], err_ids->items[i]);
				}
			}

			if (warn_count > 0) {
				printf("ℹ️  %d avertissement(s) (détails dans le log clear)\n", warn_count);
				fprintf(logs->clear, "ℹ️  %d avertissement(s)\n", warn_count);
			}
			if (skip_count > 0) {
				printf("⏭  %d vidéo(s) déjà archivée(s), ignorée(s)\n", skip_count);
				fprintf(logs->clear, "⏭  %d vidéo(s) déjà archivée(s), ignorée(s)\n", skip_count);
			}

			printf("⏱  Run time : %lds\n", secs);
			fprintf(logs->clear, "⏱  Run time : %lds\n", secs);

			printf("%s======================================%s\n\n", COL_BOLD, COL_RESET);
			fprintf(logs->clear, "======================================\n");
		}

		/* ===========================================================================
		 * EXECUTION DE COMMANDES AVEC CAPTURE LIGNE PAR LIGNE (stdout+stderr fusionnés)
		 * ======================================================================== */
		typedef void (*LineHandler)(char *line, void *ctx);

		/* out_pid (optionnel) reçoit le pid de l'enfant dès le fork, avant même que
		 * la première ligne ne soit traitée : ça permet à un handler de tuer le
		 * process en cours de route (utilisé par le mode qualité stricte). */
		static int run_and_capture(char *const argv[], LineHandler handler, void *ctx, pid_t *out_pid) {
			int pipefd[2];
			if (pipe(pipefd) != 0) { perror("pipe"); return -1; }

			pid_t pid = fork();
			if (pid < 0) { perror("fork"); return -1; }

			if (pid == 0) {
				dup2(pipefd[1], STDOUT_FILENO);
				dup2(pipefd[1], STDERR_FILENO);
				close(pipefd[0]);
				close(pipefd[1]);
				execvp(argv[0], argv);
				fprintf(stderr, "Impossible d'exécuter %s : %s\n", argv[0], strerror(errno));
				_exit(127);
			}

			if (out_pid) *out_pid = pid;

			close(pipefd[1]);
			FILE *fp = fdopen(pipefd[0], "r");

			char *line = NULL;
			size_t cap = 0;
			ssize_t len;
			while ((len = getline(&line, &cap, fp)) != -1) {
				while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
				strip_ansi(line);
				handler(line, ctx);
			}
			free(line);
			fclose(fp);

			int status;
			waitpid(pid, &status, 0);
			if (WIFEXITED(status)) return WEXITSTATUS(status);
			return -1; /* terminé par un signal (ex: SIGTERM envoyé par nous-mêmes) */
		}

		/* ----- Étape 1 : analyse de la playlist Watch Later ------------------- */
		typedef struct {
			Logs *logs;
			StrArray *ids;
		} WLCtx;

		static void wl_line_handler(char *line, void *ctxp) {
			WLCtx *ctx = ctxp;
			/* RAW, toujours, sans filtrage : c'est le rôle du log detail. */
			fprintf(ctx->logs->detail, "%s\n", line);

			if (strncmp(line, MARK_ID, strlen(MARK_ID)) == 0) {
				const char *id = line + strlen(MARK_ID);
				char tmp[12];
				if (extract_id(id, tmp)) sa_push(ctx->ids, tmp);
			}
		}

		/* ----- Étape 2 : téléchargement ---------------------------------------- */
		typedef struct {
			Logs *logs;
			StrArray error_lines;
			StrArray error_ids;
			int error_count;
			int warning_count;
			int skip_count;      /* vidéos déjà archivées, ignorées */
			int video_index;
			int is_tty;
			int progress_open;   /* une ligne de % est-elle affichée en cours ? */
			int strict_mode;     /* 1 si serveur POT indisponible : >=1080p exigé */
			int abort_requested; /* 1 si on a déclenché l'arrêt anticipé qualité */
			pid_t child_pid;     /* pid de yt-dlp, pour pouvoir le tuer en mode strict */
			char pending_format[128];
		} DLCtx;

		/* Repère les lignes du type "[info] VIDEOID: Downloading N format(s): XXX"
		 * pour enrichir le log clear avec le format effectivement retenu.
		 * Fragile (dépend du message exact de yt-dlp) : n'affecte que la richesse
		 * du log clear, jamais le téléchargement lui-même. */
		static void maybe_capture_format(DLCtx *ctx, const char *line) {
			if (strncmp(line, "[info] ", 7) != 0) return;
			const char *p = strstr(line, "Downloading ");
			if (!p) return;
			p = strstr(p, "format(s): ");
			if (!p) return;
			p += strlen("format(s): ");
			snprintf(ctx->pending_format, sizeof ctx->pending_format, "%s", p);
		}

		static void dl_line_handler(char *line, void *ctxp) {
			DLCtx *ctx = ctxp;
			/* RAW, toujours, sans filtrage. */
			fprintf(ctx->logs->detail, "%s\n", line);

			maybe_capture_format(ctx, line);

			int is_marker = strncmp(line, MARK_TITLE, strlen(MARK_TITLE)) == 0;

			if (is_marker) {
				printf("\r\033[K");
				fflush(stdout);
				ctx->progress_open = 0;

				const char *title = line + strlen(MARK_TITLE);
				ctx->video_index++;

				printf("%s▶ [%d] %s%s\n", COL_CYAN, ctx->video_index, title, COL_RESET);
				fflush(stdout);

				if (ctx->pending_format[0]) {
					fprintf(ctx->logs->clear, "▶ [%d] %s — format(s) : %s\n",
							ctx->video_index, title, ctx->pending_format);
				} else {
					fprintf(ctx->logs->clear, "▶ [%d] %s\n", ctx->video_index, title);
				}
				ctx->pending_format[0] = '\0';

			} else if (strncmp(line, "ERROR:", 6) == 0) {
				printf("\r\033[K");
				fflush(stdout);
				ctx->progress_open = 0;

				const char *msg = line + 6;
				while (*msg == ' ') msg++;
				sa_push(&ctx->error_lines, msg);

				char id[12] = "inconnu";
				const char *p = strstr(line, "[youtube] ");
				char tmp[12];
				if (p && extract_id(p + strlen("[youtube] "), tmp)) memcpy(id, tmp, sizeof tmp);
				sa_push(&ctx->error_ids, id);
				ctx->error_count++;

				printf("  %s✖ %s%s\n", COL_RED, msg, COL_RESET);
				fflush(stdout);
				fprintf(ctx->logs->clear, "  ✖ %s (ID : %s)\n", msg, id);

			/* Mode strict (serveur POT indisponible) : la première erreur
			 * rencontrée déclenche l'arrêt PROPRE de tout le run, pas juste le
			 * passage à la vidéo suivante. On considère qu'on ne sait plus
			 * garantir la qualité minimale, donc mieux vaut s'arrêter que
			 * continuer à l'aveugle. */
			if (ctx->strict_mode && !ctx->abort_requested) {
				ctx->abort_requested = 1;
				fprintf(ctx->logs->clear,
					"  ⛔ Serveur POT indisponible et qualité >= %dp non garantie pour cette "
					"vidéo : arrêt propre du programme demandé.\n", MIN_HEIGHT_STRICT);
				fprintf(ctx->logs->detail,
					"[prog] abandon mode strict déclenché sur ID %s -> SIGTERM vers yt-dlp (pid %d)\n",
					id, (int)ctx->child_pid);
				printf("  %s⛔ Qualité >= %dp non garantie sans serveur POT : arrêt du programme.%s\n",
					   COL_RED, MIN_HEIGHT_STRICT, COL_RESET);
				fflush(stdout);
				if (ctx->child_pid > 0) kill(ctx->child_pid, SIGTERM);
			}

		} else if (strncmp(line, "WARNING:", 8) == 0) {
			ctx->warning_count++;
			fprintf(ctx->logs->clear, "  ⚠ %s\n", line);

		} else if (strstr(line, "has already been recorded in the archive")) {
			ctx->skip_count++;

		} else if (strncmp(line, "[download] 100%", strlen("[download] 100%")) == 0) {
			if (ctx->is_tty && ctx->progress_open) {
				printf("\n"); /* fige la ligne de progression au lieu de l'effacer */
				ctx->progress_open = 0;
			}
			fprintf(ctx->logs->clear, "   ✓ Terminé (%s)\n", line + strlen("[download] "));

		} else if (strstr(line, "[download]") && strchr(line, '%')) {
			if (ctx->is_tty) {
				printf("\r\033[K%s", line);
				fflush(stdout);
				ctx->progress_open = 1;
			}
		}
	}

	/* ===========================================================================
	 * SERVEUR POT (node) + attente de disponibilité
	 * ======================================================================== */
	static volatile pid_t g_pot_pid = -1;
	static Logs *g_logs = NULL; /* pour un flush best-effort en cas de signal */

	static void on_signal(int sig) {
		(void)sig;
		/* Best-effort : fflush n'est pas strictement async-signal-safe, mais
		   limite le risque de perdre la dernière ligne en cas de Ctrl-C. Une
		   version rigoureuse utiliserait un self-pipe pour repousser tout ceci
		   dans la boucle principale. */
		if (g_logs) {
			if (g_logs->detail) fflush(g_logs->detail);
			if (g_logs->clear) fflush(g_logs->clear);
		}
		if (g_pot_pid > 0) kill(g_pot_pid, SIGTERM);
		_exit(130);
	}

	static pid_t start_pot_server(const char *pot_log_path) {
		pid_t pid = fork();
		if (pid < 0) { perror("fork (pot)"); return -1; }
		if (pid == 0) {
			if (chdir(POT_DIR) != 0) _exit(126);
			int fd = open(pot_log_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
			if (fd >= 0) {
				dup2(fd, STDOUT_FILENO);
				dup2(fd, STDERR_FILENO);
				close(fd);
			}
			char *argv[] = { (char *)"node", (char *)"build/main.js", (char *)"--port", (char *)POT_PORT, NULL };
			execvp(argv[0], argv);
			_exit(127);
		}
		return pid;
	}

	/* Renvoie 1 si le serveur a répondu avant max_tries secondes (out_elapsed
	 * reçoit le nombre de secondes attendues), 0 sinon. */
	static int wait_pot_ready(const char *port, int max_tries, int *out_elapsed) {
		char url[64];
		snprintf(url, sizeof url, "http://localhost:%s", port);

		for (int i = 0; i < max_tries; i++) {
			pid_t pid = fork();
			if (pid == 0) {
				int devnull = open("/dev/null", O_WRONLY);
				dup2(devnull, STDOUT_FILENO);
				dup2(devnull, STDERR_FILENO);
				char *argv[] = { (char *)"curl", (char *)"-s", (char *)"-o", (char *)"/dev/null", url, NULL };
				execvp("curl", argv);
				_exit(127);
			}
			int status;
			waitpid(pid, &status, 0);
			if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
				if (out_elapsed) *out_elapsed = i + 1;
				return 1;
			}
			printf(".");
			fflush(stdout);
			sleep(1);
		}
		if (out_elapsed) *out_elapsed = max_tries;
		return 0;
	}

	/* ===========================================================================
	 * MAIN
	 * ======================================================================== */
	int main(void) {
		int is_tty = isatty(STDOUT_FILENO);
		init_colors(is_tty);

		signal(SIGINT, on_signal);
		signal(SIGTERM, on_signal);

		/* 0. Purge des logs existants (avant de créer le dossier de ce run). */
		cleanup_old_logs();

    /* 1. Préparation des fichiers de logs du run courant. */
    char *ts = now_timestamp();
    char log_dir[2048];
    snprintf(log_dir, sizeof log_dir, "%s/%s", LOGS_DIR, ts);

    if (mkdir_p(log_dir) != 0) {
        fprintf(stderr, "Impossible de créer le dossier de logs %s : %s\n", log_dir, strerror(errno));
        free(ts);
        return 1;
    }
    /* 0700 : le dossier peut contenir des cookies de session (cf. detail log). */
    chmod(log_dir, S_IRWXU);

    char detail_path[2200], clear_path[2200], pot_path[2200];
    snprintf(detail_path, sizeof detail_path, "%s/%s_log_detail.txt", log_dir, ts);
    snprintf(clear_path, sizeof clear_path, "%s/%s_log_clear.txt", log_dir, ts);
    snprintf(pot_path, sizeof pot_path, "%s/%s_pot_server.txt", log_dir, ts);

    Logs logs;
    logs.detail = fopen(detail_path, "w");
    logs.clear  = fopen(clear_path, "w");
    if (!logs.detail || !logs.clear) {
        perror("Impossible d'ouvrir les fichiers de logs");
        free(ts);
        return 1;
    }
    setvbuf(logs.detail, NULL, _IOLBF, 0);
    setvbuf(logs.clear, NULL, _IOLBF, 0);
    /* 0600 : le detail log peut contenir des cookies de session en clair. */
    chmod(detail_path, S_IRUSR | S_IWUSR);
    chmod(clear_path, S_IRUSR | S_IWUSR);

    g_logs = &logs;

    fprintf(logs.detail, "=== LOG DETAIL (raw -- usage debug / IA) -- run %s ===\n", ts);
    print_banner(&logs);

    /* 2. Serveur POT. */
    print_section(&logs, "Démarrage du serveur POT...");
    fprintf(logs.detail, "\n--- Serveur POT (voir aussi %s_pot_server.txt) ---\n", ts);

    pid_t pot_pid = start_pot_server(pot_path);
    g_pot_pid = pot_pid;

    printf("   Attente du serveur POT (jusqu'à %ds)", POT_WAIT_MAX_SECONDS);
    fflush(stdout);
    int pot_elapsed = 0;
    int pot_ready = wait_pot_ready(POT_PORT, POT_WAIT_MAX_SECONDS, &pot_elapsed);
    int strict_mode = !pot_ready;

    if (pot_ready) {
        printf(" OK (%ds)\n", pot_elapsed);
        fprintf(logs.clear, "  Serveur POT prêt après %ds.\n", pot_elapsed);
        fprintf(logs.detail, "[prog] Serveur POT prêt après %ds (strict_mode=0).\n", pot_elapsed);
    } else {
        printf(" %s(aucune réponse après %ds)%s\n", COL_YELLOW, pot_elapsed, COL_RESET);
        fprintf(logs.clear,
            "  ⚠ Serveur POT indisponible après %ds : mode qualité stricte activé "
            "(>= %dp exigé ; arrêt propre du programme si non satisfaisable).\n",
            pot_elapsed, MIN_HEIGHT_STRICT);
        fprintf(logs.detail, "[prog] Serveur POT indisponible après %ds (strict_mode=1).\n", pot_elapsed);
        /* Il est peut-être encore en train de démarrer : on l'arrête plutôt
           que de laisser un process node orphelin pour un run déjà basculé
           en mode dégradé/strict. */
        if (pot_pid > 0) {
            kill(pot_pid, SIGTERM);
            int st;
            waitpid(pot_pid, &st, 0);
            g_pot_pid = -1;
            pot_pid = -1;
        }
    }

    /* 3. Analyse de la Watch Later. */
    print_section(&logs, "Analyse de Watch Later...");
    fprintf(logs.detail, "\n--- Analyse Watch Later ---\n");

    StrArray wl_ids;
    sa_init(&wl_ids);
    WLCtx wctx = { &logs, &wl_ids };

    char *wl_argv[] = {
        (char *)"yt-dlp",
        (char *)"--flat-playlist",
        (char *)"--verbose",
        (char *)"--print-traffic",
        (char *)"--print", (char *)MARK_ID "%(id)s",
        (char *)"--cookies-from-browser", (char *)COOKIE_BROWSER,
        (char *)WL_URL,
        NULL
    };
    int wl_status = run_and_capture(wl_argv, wl_line_handler, &wctx, NULL);

    int fatal = 0;
    if (wl_status == 127) {
        fprintf(stderr, "%syt-dlp introuvable dans le PATH.%s\n", COL_RED, COL_RESET);
        fprintf(logs.clear, "✖ yt-dlp introuvable dans le PATH -- arrêt.\n");
        fprintf(logs.detail, "[prog] yt-dlp introuvable (étape analyse) -- arrêt.\n");
        fatal = 1;
    }

    sa_sort_unique(&wl_ids);
    int total = (int)wl_ids.count;
    int archived_in_wl = fatal ? 0 : count_archived(&wl_ids);

    if (!fatal) {
        printf("   Vidéos dans Watch Later : %s%d%s\n", COL_BOLD, total, COL_RESET);
        printf("   Déjà archivées          : %d\n", archived_in_wl);
        printf("   À télécharger (estim.)  : %s%d%s\n", COL_BOLD, total - archived_in_wl, COL_RESET);
        fprintf(logs.clear,
            "  %d vidéo(s) dans Watch Later, %d déjà archivée(s), %d à télécharger (estim.)\n",
            total, archived_in_wl, total - archived_in_wl);
    }

    /* 4. Téléchargement. */
    DLCtx dctx;
    memset(&dctx, 0, sizeof dctx);
    dctx.logs = &logs;
    dctx.is_tty = is_tty;
    dctx.strict_mode = strict_mode;
    dctx.child_pid = -1;
    sa_init(&dctx.error_lines);
    sa_init(&dctx.error_ids);

    int dl_status = -1;
    time_t t0 = time(NULL), t1 = t0;

    if (!fatal) {
        print_section(&logs, "Téléchargement...");
        fprintf(logs.clear, "\n▶ Téléchargement... (mode %s)\n",
                strict_mode ? "strict : >=1080p exigé" : "normal");
        fprintf(logs.detail, "\n--- Téléchargement (strict_mode=%d) ---\n", strict_mode);

        const char *format_selector = strict_mode ? FORMAT_SELECTOR_STRICT : FORMAT_SELECTOR_NORMAL;
        char output_template[2200];
        snprintf(output_template, sizeof output_template, "%s/%%(title)s.%%(ext)s", BASE_DIR);

        t0 = time(NULL);
        char *dl_argv[] = {
            (char *)"yt-dlp",
            (char *)"--cookies-from-browser", (char *)COOKIE_BROWSER,
            (char *)"--extractor-args", (char *)"youtube:player_client=mweb",
            (char *)"-f", (char *)format_selector,
            (char *)"--merge-output-format", (char *)"mkv",
            (char *)"--download-archive", (char *)ARCHIVE_FILE,
            (char *)"--ignore-errors",
            (char *)"--no-abort-on-error",
            (char *)"--newline",
            (char *)"--progress",
            //(char *)"--restrict-filenames",
            (char *)"--no-part",
            (char *)"--verbose",
            (char *)"--print-traffic",
            (char *)"--print", (char *)"before_dl:" MARK_TITLE "%(title)s",
            (char *)"--concurrent-fragments", (char *)"8",
            (char *)"-o", output_template,
            (char *)WL_URL,
            NULL
        };
        dl_status = run_and_capture(dl_argv, dl_line_handler, &dctx, &dctx.child_pid);

        if (dctx.progress_open) {
            printf("\r\033[K");
            fflush(stdout);
            dctx.progress_open = 0;
        }
        if (dl_status == 127) {
            fprintf(stderr, "%syt-dlp introuvable dans le PATH.%s\n", COL_RED, COL_RESET);
            fprintf(logs.clear, "✖ yt-dlp introuvable dans le PATH (étape téléchargement) -- arrêt.\n");
            fprintf(logs.detail, "[prog] yt-dlp introuvable (étape téléchargement) -- arrêt.\n");
        }
        t1 = time(NULL);
    }

    /* 5. Résumé final. */
    if (fatal) {
        printf("\n%s======================================%s\n", COL_BOLD, COL_RESET);
        printf("%s✖ Arrêt : yt-dlp est introuvable dans le PATH.%s\n", COL_RED, COL_RESET);
        printf("%s======================================%s\n\n", COL_BOLD, COL_RESET);
    } else {
        print_summary(&logs, dl_status, dctx.error_count, dctx.warning_count, dctx.skip_count,
                      &dctx.error_lines, &dctx.error_ids, (long)(t1 - t0), dctx.abort_requested);
    }

    int has_error = fatal || (wl_status != 0) || (dl_status != 0) || (dctx.error_count > 0);

    fprintf(logs.detail, "\n=== Fin -- code retour %d, %d erreur(s), %d avertissement(s), statut=%s ===\n",
            dl_status, dctx.error_count, dctx.warning_count, has_error ? "ERR" : "OK");
    fprintf(logs.clear, "\n=== Fin -- statut : %s ===\n", has_error ? "ERREUR" : "OK");

    fclose(logs.detail);
    fclose(logs.clear);
    g_logs = NULL;

    /* 6. Arrêt propre du serveur POT s'il tournait encore. */
    if (pot_pid > 0) {
        kill(pot_pid, SIGTERM);
        int st;
        waitpid(pot_pid, &st, 0);
    }
    g_pot_pid = -1;

/* 7. Renommage du dossier selon le statut, pour piloter la purge future. */
    char final_log_dir[2100];
    char log_dir_name[128]; /* juste le nom du dossier (pas le chemin complet), pour un affichage propre */
    snprintf(final_log_dir, sizeof final_log_dir, "%s_%s", log_dir, has_error ? "ERR" : "OK");
    if (rename(log_dir, final_log_dir) != 0) {
        fprintf(stderr, "Avertissement : impossible de renommer %s (%s)\n", log_dir, strerror(errno));
        snprintf(final_log_dir, sizeof final_log_dir, "%s", log_dir);
        snprintf(log_dir_name, sizeof log_dir_name, "%s", ts); /* renommage raté : nom resté sans suffixe */
    } else {
        snprintf(log_dir_name, sizeof log_dir_name, "%s_%s", ts, has_error ? "ERR" : "OK");
    }

    printf("%sLogs :%s [%s]\n\n", COL_DIM, COL_RESET, log_dir_name);
    sa_free(&wl_ids);
    sa_free(&dctx.error_lines);
    sa_free(&dctx.error_ids);
    free(ts);

    return has_error ? 1 : 0;
}
