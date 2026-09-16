#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <limits.h>

#define MAX_INDEX_LINES 8000
#define MAX_PROJECTS 1000

typedef struct {
    char name[128];
    char rel_path[PATH_MAX];
    char abs_path[PATH_MAX];
    char dir[PATH_MAX];
    char type[64];
    char desc[2048];
    int indent;
    int line_idx;
    int parent_idx;
    int depth;
    bool is_leaf;
} project_info;

static char *index_lines[MAX_INDEX_LINES];
static int num_index_lines = 0;

static project_info projects[MAX_PROJECTS];
static int num_projects = 0;

static char updind_index_path[PATH_MAX] = {0};

static void save_index_md_to_disk(void) {
    if (updind_index_path[0] == '\0') return;
    FILE *fp_out = fopen(updind_index_path, "w");
    if (!fp_out) {
        fprintf(stderr, "ds4-agent: failed to open INDEX.md for writing\n");
        return;
    }
    for (int i = 0; i < num_index_lines; i++) {
        fputs(index_lines[i], fp_out);
    }
    fclose(fp_out);
}

/* Helper to replace all occurrences of a string */
static void replace_string(const char *orig, const char *search, const char *replace, char *dest, size_t dest_max) {
    dest[0] = '\0';
    size_t dest_len = 0;
    size_t search_len = strlen(search);
    const char *p = orig;
    while (*p) {
        const char *found = strstr(p, search);
        if (found) {
            size_t prefix_len = found - p;
            if (dest_len + prefix_len < dest_max) {
                memcpy(dest + dest_len, p, prefix_len);
                dest_len += prefix_len;
                dest[dest_len] = '\0';
            }
            size_t replace_len = strlen(replace);
            if (dest_len + replace_len < dest_max) {
                memcpy(dest + dest_len, replace, replace_len);
                dest_len += replace_len;
                dest[dest_len] = '\0';
            }
            p = found + search_len;
        } else {
            size_t left_len = strlen(p);
            if (dest_len + left_len < dest_max) {
                memcpy(dest + dest_len, p, left_len);
                dest_len += left_len;
                dest[dest_len] = '\0';
            }
            break;
        }
    }
}

/* Helper to check if a description is a placeholder/empty */
static bool is_placeholder(const char *text) {
    if (!text || text[0] == '\0') return true;
    int i = 0;
    while (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n') i++;
    if (text[i] == '\0') return true;

    char lower[2048] = {0};
    for (int j = 0; text[i+j] && j < 2000; j++) {
        lower[j] = tolower((unsigned char)text[i+j]);
    }
    if (strstr(lower, "<placeholder") != NULL) return true;
    if (strstr(lower, "<sintesi autogenerata") != NULL) return true;
    if (strstr(lower, "<sintesi recuperata") != NULL) return true;
    if (strstr(lower, "sintesi non disponibile") != NULL) return true;
    if (strstr(lower, "stub") != NULL) return true;
    if (strstr(lower, "<｜") != NULL) return true;
    if (strstr(lower, "<|") != NULL) return true;
    if (strstr(lower, "<│") != NULL) return true;
    if (strstr(lower, "<∣") != NULL) return true;
    if (strstr(lower, "<❘") != NULL) return true;
    if (strstr(lower, "sentence") != NULL && strstr(lower, "<") != NULL) return true;
    return false;
}

/* Parse a line of INDEX.md */
static bool parse_index_line(const char *line, char *name, char *path, char *type, char *desc, int *indent) {
    int i = 0;
    while (line[i] == ' ' || line[i] == '\t') i++;
    *indent = i;
    if (line[i] != '-' || line[i+1] != ' ') return false;
    i += 2;
    if (line[i] != '[') return false;
    i++;
    int name_start = i;
    while (line[i] && line[i] != ']') i++;
    if (!line[i]) return false;
    int name_len = i - name_start;
    i++; // skip ']'
    if (line[i] != '(') return false;
    i++;
    int path_start = i;
    while (line[i] && line[i] != ')') i++;
    if (!line[i]) return false;
    int path_len = i - path_start;
    i++; // skip ')'
    
    while (line[i] && line[i] == ' ') i++;
    if (line[i] != '[') return false;
    i++;
    int type_start = i;
    while (line[i] && line[i] != ']') i++;
    if (!line[i]) return false;
    int type_len = i - type_start;
    i++; // skip ']'
    
    while (line[i] && line[i] == ' ') i++;
    if (line[i] != '-' || line[i+1] != ' ') return false;
    i += 2;
    int desc_start = i;
    
    if (name_len >= 128) name_len = 127;
    strncpy(name, line + name_start, name_len);
    name[name_len] = '\0';
    if (path_len >= PATH_MAX) path_len = PATH_MAX - 1;
    strncpy(path, line + path_start, path_len);
    path[path_len] = '\0';
    if (type_len >= 64) type_len = 63;
    strncpy(type, line + type_start, type_len);
    type[type_len] = '\0';
    strncpy(desc, line + desc_start, 2047);
    desc[2047] = '\0';
    
    int len = strlen(desc);
    while (len > 0 && (desc[len-1] == '\r' || desc[len-1] == '\n')) {
        desc[len-1] = '\0';
        len--;
    }
    return true;
}

/* Parse a documentation markdown link line */
static bool parse_doc_line(const char *line, char *rel_path, char *desc) {
    int i = 0;
    while (line[i] == ' ' || line[i] == '\t') i++;
    if (line[i] != '-' || line[i+1] != ' ') return false;
    i += 2;
    if (line[i] != '[') return false;
    i++;
    int path_start = i;
    while (line[i] && line[i] != ']') i++;
    if (!line[i]) return false;
    int path_len = i - path_start;
    i++; // skip ']'
    if (line[i] != '(') return false;
    i++;
    while (line[i] && line[i] != ')') i++;
    if (!line[i]) return false;
    i++; // skip ')'
    if (line[i] != ':' || line[i+1] != ' ') return false;
    i += 2;
    
    if (path_len >= PATH_MAX) path_len = PATH_MAX - 1;
    strncpy(rel_path, line + path_start, path_len);
    rel_path[path_len] = '\0';
    strncpy(desc, line + i, 2047);
    desc[2047] = '\0';
    
    int len = strlen(desc);
    while (len > 0 && (desc[len-1] == '\r' || desc[len-1] == '\n')) {
        desc[len-1] = '\0';
        len--;
    }
    return true;
}

/* Parse a source file line */
static bool parse_src_line(const char *line, char *file_name, char *desc) {
    int i = 0;
    while (line[i] == ' ' || line[i] == '\t') i++;
    if (line[i] != '-' || line[i+1] != ' ') return false;
    i += 2;
    if (line[i] != '`') return false;
    i++;
    int file_start = i;
    while (line[i] && line[i] != '`') i++;
    if (!line[i]) return false;
    int file_len = i - file_start;
    i++; // skip '`'
    if (line[i] != ':' || line[i+1] != ' ') return false;
    i += 2;
    
    if (file_len >= PATH_MAX) file_len = PATH_MAX - 1;
    strncpy(file_name, line + file_start, file_len);
    file_name[file_len] = '\0';
    strncpy(desc, line + i, 2047);
    desc[2047] = '\0';
    
    int len = strlen(desc);
    while (len > 0 && (desc[len-1] == '\r' || desc[len-1] == '\n')) {
        desc[len-1] = '\0';
        len--;
    }
    return true;
}

/* Helper to decode a range of tokens to text */
static char *decode_token_range(ds4_engine *engine, const ds4_tokens *tokens, int start, int end) {
    size_t cap = 2048;
    char *buf = xmalloc(cap);
    size_t len = 0;
    buf[0] = '\0';
    for (int i = start; i < end; i++) {
        size_t t_len = 0;
        char *text = ds4_token_text(engine, tokens->v[i], &t_len);
        if (text) {
            if (len + t_len >= cap) {
                cap = (len + t_len) * 2;
                buf = xrealloc(buf, cap);
            }
            memcpy(buf + len, text, t_len);
            len += t_len;
            buf[len] = '\0';
            free(text);
        }
    }
    return buf;
}

/* Helper to truncate special EOS tags (including vertical bar variations) and trim trailing whitespace */
static void truncate_eos_tags(char *str) {
    if (!str) return;
    char *p = str;
    while (*p) {
        if (*p == '<') {
            char temp[128] = {0};
            strncpy(temp, p, 127);
            for (int i = 0; temp[i]; i++) {
                temp[i] = tolower((unsigned char)temp[i]);
            }
            bool match = false;
            if (temp[1] == '|') match = true;
            else if (strncmp(temp + 1, "\xef\xbd\x9c", 3) == 0) match = true;
            else if (strncmp(temp + 1, "\xe2\x94\x82", 3) == 0) match = true;
            else if (strncmp(temp + 1, "\xe2\x88\xa3", 3) == 0) match = true;
            else if (strncmp(temp + 1, "\xe2\x9d\x98", 3) == 0) match = true;
            else if (strstr(temp, "assistant") != NULL) match = true;
            else if (strstr(temp, "dsml") != NULL) match = true;
            else if (strstr(temp, "sentence") != NULL) match = true;
            else if (strstr(temp, "eos") != NULL) match = true;
            else if (strstr(temp, "end") != NULL) match = true;
            else if (strstr(temp, "tool_calls") != NULL) match = true;
            
            if (match) {
                *p = '\0';
                break;
            }
        }
        p++;
    }
    int len = strlen(str);
    while (len > 0 && (str[len-1] == ' ' || str[len-1] == '\t' || str[len-1] == '\r' || str[len-1] == '\n')) {
        str[len-1] = '\0';
        len--;
    }
}

/* Helper to extract actual assistant text response, skipping headers/think block */
static char *extract_assistant_response(const char *full_text) {
    const char *assist = strstr(full_text, "<｜Assistant｜>");
    if (!assist) {
        assist = strstr(full_text, "Assistant");
        if (!assist) assist = full_text;
        else assist += 9;
    } else {
        assist += strlen("<｜Assistant｜>");
    }
    
    const char *think_start = strstr(assist, "<think>");
    if (think_start && think_start < assist + 100) {
        const char *think_end = strstr(think_start, "</think>");
        if (think_end) {
            assist = think_end + strlen("</think>");
        }
    }
    
    while (*assist == ' ' || *assist == '\t' || *assist == '\r' || *assist == '\n') {
        assist++;
    }
    
    char *res = xstrdup(assist);
    truncate_eos_tags(res);
    return res;
}

/* Run greedy generation safely using the worker thread queue and status checks */
static int generate_text_via_worker(agent_worker *w, const char *prompt, char *out_buf, size_t out_max) {
    // Wait for worker thread to be idle
    while (true) {
        pthread_mutex_lock(&w->mu);
        bool idle = (w->status.state == AGENT_WORKER_IDLE);
        pthread_mutex_unlock(&w->mu);
        if (idle) break;
        usleep(10000);
    }
    
    // Reset to system prompt (equivalent to /new)
    char reset_err[160];
    if (!agent_worker_reset_to_sysprompt(w, reset_err, sizeof(reset_err))) {
        fprintf(stderr, "ds4-agent: reset to system prompt failed: %s\n", reset_err);
        return -1;
    }
    
    // Record length before submit
    pthread_mutex_lock(&w->mu);
    int len_before = w->transcript.len;
    pthread_mutex_unlock(&w->mu);
    
    // Submit prompt
    if (!worker_submit(w, prompt)) {
        fprintf(stderr, "ds4-agent: failed to submit prompt to worker\n");
        return -1;
    }
    
    // Wait for worker thread to finish processing
    usleep(50000);
    while (true) {
        char *out = NULL;
        size_t out_len = 0;
        agent_status st = {0};
        worker_consume(w, &out, &out_len, &st);
        if (out && out_len) {
            write(STDOUT_FILENO, out, out_len);
            fflush(stdout);
            free(out);
        }
        if (st.state == AGENT_WORKER_IDLE || st.state == AGENT_WORKER_ERROR || st.state == AGENT_WORKER_STOPPED) {
            break;
        }
        usleep(10000);
    }
    
    // Decode generated tokens (specifically the final assistant response to exclude any tool calls)
    pthread_mutex_lock(&w->mu);
    int last_assistant_idx = len_before;
    int assistant_id = ds4_token_assistant(w->engine);
    for (int i = w->transcript.len - 1; i >= len_before; i--) {
        if (w->transcript.v[i] == assistant_id) {
            last_assistant_idx = i;
            break;
        }
    }
    int end_idx = w->transcript.len;
    if (end_idx > last_assistant_idx && w->transcript.v[end_idx - 1] == ds4_token_eos(w->engine)) {
        end_idx--;
    }
    char *full_decoded = decode_token_range(w->engine, &w->transcript, last_assistant_idx, end_idx);
    pthread_mutex_unlock(&w->mu);
    
    // Extract assistant response
    char *response = extract_assistant_response(full_decoded);
    free(full_decoded);
    
    strncpy(out_buf, response, out_max - 1);
    out_buf[out_max - 1] = '\0';
    free(response);
    
    return 0;
}

/* Gather summaries of sottomoduli/descendant projects in INDEX.md */
static void get_descendant_summaries(int proj_idx, char *out_buf, size_t max_len) {
    out_buf[0] = '\0';
    size_t out_len = 0;
    const char *p_dir = projects[proj_idx].dir;
    int p_dir_len = strlen(p_dir);
    for (int i = 0; i < num_projects; i++) {
        if (i == proj_idx) continue;
        const char *c_dir = projects[i].dir;
        if (strncmp(c_dir, p_dir, p_dir_len) == 0 && c_dir[p_dir_len] == '/') {
            char line[1024];
            snprintf(line, sizeof(line), "- %s: %s\n", projects[i].name, projects[i].desc);
            size_t line_len = strlen(line);
            if (out_len + line_len < max_len) {
                strcpy(out_buf + out_len, line);
                out_len += line_len;
            }
        }
    }
}

/* Scan project folder for other .md files */
static void get_project_md_files_list(const char *dir_path, const char *self_md_name, char *out_buf, size_t max_len) {
    out_buf[0] = '\0';
    size_t out_len = 0;
    DIR *dir = opendir(dir_path);
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
        int name_len = strlen(name);
        if (name_len > 3 && strcmp(name + name_len - 3, ".md") == 0) {
            if (strcmp(name, self_md_name) != 0) {
                char line[512];
                snprintf(line, sizeof(line), "- %s\n", name);
                size_t line_len = strlen(line);
                if (out_len + line_len < max_len) {
                    strcpy(out_buf + out_len, line);
                    out_len += line_len;
                }
            }
        }
    }
    closedir(dir);
}

/* Load usedlibraryfrom-[proj].json content */
static void get_project_dependency_str(const char *dir_path, const char *proj_name, char *out_buf, size_t max_len) {
    char json_path[PATH_MAX];
    snprintf(json_path, sizeof(json_path), "%s/usedlibraryfrom-%s.json", dir_path, proj_name);
    FILE *fp = fopen(json_path, "r");
    if (!fp) {
        strcpy(out_buf, "Nessuna dipendenza dichiarata.");
        return;
    }
    size_t read_bytes = fread(out_buf, 1, max_len - 1, fp);
    out_buf[read_bytes] = '\0';
    fclose(fp);
}

/* Pre-scan the project's markdown file to check if it contains any placeholders/stubs */
static bool file_has_placeholders(const char *path, const char *proj_type) {
    FILE *fp = fopen(path, "r");
    if (!fp) return false;
    
    char line[4096];
    bool in_desc_block = false;
    char desc_accum[8192] = {0};
    bool has_placeholder = false;
    
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "**Descrizione generale del progetto:**", 38) == 0) {
            in_desc_block = true;
            desc_accum[0] = '\0';
            continue;
        }
        
        if (in_desc_block) {
            if (line[0] == '#' || strncmp(line, "**", 2) == 0) {
                in_desc_block = false;
                if (is_placeholder(desc_accum)) {
                    has_placeholder = true;
                    break;
                }
            } else {
                strncat(desc_accum, line, sizeof(desc_accum) - strlen(desc_accum) - 1);
                continue;
            }
        }
        
        char rel_path[PATH_MAX];
        char desc[2048];
        if (parse_doc_line(line, rel_path, desc)) {
            if (is_placeholder(desc) && strstr(proj_type, "External Library") == NULL) {
                has_placeholder = true;
                break;
            }
        } else if (parse_src_line(line, rel_path, desc)) {
            if (is_placeholder(desc) && strstr(proj_type, "External Library") == NULL) {
                has_placeholder = true;
                break;
            }
        }
    }
    
    if (in_desc_block && is_placeholder(desc_accum)) {
        has_placeholder = true;
    }
    
    fclose(fp);
    return has_placeholder;
}

/* Parse a project's markdown file, generate descriptions for placeholders, and save changes */
static bool process_project_markdown(project_info *proj, agent_worker *w, const char *base_dir) {
    if (w->cfg->dryrun) {
        FILE *fp = fopen(proj->abs_path, "r");
        if (!fp) return false;
        
        char line[4096];
        bool in_desc_block = false;
        char desc_accum[8192] = {0};
        bool would_update_any = false;
        
        while (fgets(line, sizeof(line), fp)) {
            if (strncmp(line, "**Descrizione generale del progetto:**", 38) == 0) {
                in_desc_block = true;
                desc_accum[0] = '\0';
                continue;
            }
            
            if (in_desc_block) {
                if (line[0] == '#' || strncmp(line, "**", 2) == 0) {
                    in_desc_block = false;
                    if (is_placeholder(desc_accum)) {
                        printf("[DRYRUN] Genero descrizione generale per %s\n", proj->name);
                        would_update_any = true;
                    }
                } else {
                    strncat(desc_accum, line, sizeof(desc_accum) - strlen(desc_accum) - 1);
                    continue;
                }
            }
            
            char rel_path[PATH_MAX];
            char desc[2048];
            if (parse_doc_line(line, rel_path, desc)) {
                if (is_placeholder(desc) && strstr(proj->type, "External Library") == NULL) {
                    printf("[DRYRUN] Genero descrizione file documentazione %s/%s\n", proj->name, rel_path);
                    would_update_any = true;
                }
            } else if (parse_src_line(line, rel_path, desc)) {
                if (is_placeholder(desc) && strstr(proj->type, "External Library") == NULL) {
                    printf("[DRYRUN] Genero descrizione file sorgente %s/%s\n", proj->name, rel_path);
                    would_update_any = true;
                }
            }
        }
        
        if (in_desc_block && is_placeholder(desc_accum)) {
            printf("[DRYRUN] Genero descrizione generale per %s\n", proj->name);
            would_update_any = true;
        }
        
        fclose(fp);
        if (would_update_any) {
            printf("[DRYRUN] Scrivo file markdown %s\n", proj->abs_path);
        }
        return would_update_any;
    }

    if (!file_has_placeholders(proj->abs_path, proj->type)) {
        return false;
    }

    FILE *fp_in = fopen(proj->abs_path, "r");
    if (!fp_in) {
        fprintf(stderr, "ds4-agent: failed to open %s for reading\n", proj->abs_path);
        return false;
    }
    char tmp_path[PATH_MAX];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", proj->abs_path);
    FILE *fp_out = fopen(tmp_path, "w");
    if (!fp_out) {
        fprintf(stderr, "ds4-agent: failed to open %s for writing\n", tmp_path);
        fclose(fp_in);
        return false;
    }

    char filedesc_template[4096] = {0};
    char projdesc_template[4096] = {0};
    
    char path_fd[PATH_MAX];
    snprintf(path_fd, sizeof(path_fd), "%s/filedescription.txt", base_dir);
    FILE *fp_fd = fopen(path_fd, "r");
    if (fp_fd) {
        size_t n = fread(filedesc_template, 1, sizeof(filedesc_template) - 1, fp_fd);
        filedesc_template[n] = '\0';
        fclose(fp_fd);
    }
    if (filedesc_template[0] == '\0') {
        strcpy(filedesc_template, 
            "Sei un analista del software esperto. Il tuo compito è generare una descrizione sintetica di una riga per il file sorgente specificato.\n\n"
            "Informazioni sul Progetto:\n"
            "- Elenco dei file di documentazione markdown (.md) presenti nel progetto:\n"
            "[mdfiles]\n\n"
            "- File delle dipendenze del progetto (JSON o lista):\n"
            "[dependency]\n\n"
            "File sorgente da analizzare:\n"
            "[pathfile]\n\n"
            "Generare una descrizione in massimo 1-2 frasi sul ruolo e sullo scopo del file '[pathfile]' all'interno del progetto. Sii preciso, tecnico e conciso. Rispondere solo con la descrizione, senza preamboli."
        );
    }

    char path_pd[PATH_MAX];
    snprintf(path_pd, sizeof(path_pd), "%s/descrizioneprogetto.txt", base_dir);
    FILE *fp_pd = fopen(path_pd, "r");
    if (fp_pd) {
        size_t n = fread(projdesc_template, 1, sizeof(projdesc_template) - 1, fp_pd);
        projdesc_template[n] = '\0';
        fclose(fp_pd);
    }
    if (projdesc_template[0] == '\0') {
        strcpy(projdesc_template, 
            "Sei un analista del software esperto. Il tuo compito è fornire una descrizione generale sintetica per il progetto situato in '[pathfilemdelprogetto]'.\n\n"
            "Scrivere una descrizione chiara, tecnica e professionale di massimo 2-3 frasi sullo scopo principale di questo modulo/progetto, sulle sue dipendenze e sul suo ruolo all'interno del sistema complessivo. Rispondere solo con la descrizione, senza preamboli."
        );
    }

    char line[4096];
    char desc_accum[8192] = {0};
    bool in_desc_block = false;

    char md_files_str[2048] = {0};
    char dependency_str[2048] = {0};
    
    char *self_md_name = strrchr(proj->abs_path, '/');
    self_md_name = self_md_name ? self_md_name + 1 : proj->name;
    
    get_project_md_files_list(proj->dir, self_md_name, md_files_str, sizeof(md_files_str));
    get_project_dependency_str(proj->dir, proj->name, dependency_str, sizeof(dependency_str));

    while (fgets(line, sizeof(line), fp_in)) {
        if (strncmp(line, "**Descrizione generale del progetto:**", 38) == 0) {
            in_desc_block = true;
            fprintf(fp_out, "%s", line);
            desc_accum[0] = '\0';
            continue;
        }

        if (in_desc_block) {
            if (line[0] == '#' || strncmp(line, "**", 2) == 0) {
                in_desc_block = false;
                if (is_placeholder(desc_accum)) {
                    char generated[2048] = {0};
                    if (proj->is_leaf) {
                        char prompt[4096];
                        char rel_md_path[PATH_MAX];
                        strcpy(rel_md_path, proj->abs_path);
                        int base_len = strlen(base_dir);
                        if (strncmp(rel_md_path, base_dir, base_len) == 0) {
                            memmove(rel_md_path, rel_md_path + base_len + 1, strlen(rel_md_path) - base_len);
                        }
                        replace_string(projdesc_template, "[pathfilemdelprogetto]", rel_md_path, prompt, sizeof(prompt));
                        printf("--> Generando descrizione del progetto leaf per %s...\n", proj->name);
                        generate_text_via_worker(w, prompt, generated, sizeof(generated));
                    } else {
                        char child_descriptions[8000] = {0};
                        get_descendant_summaries(proj - projects, child_descriptions, sizeof(child_descriptions));
                        char prompt[10000];
                        snprintf(prompt, sizeof(prompt), 
                            "Sei un analista del software esperto. Il tuo compito è fornire una descrizione generale sintetica per il progetto superiore a partire delle descrizioni dei suoi sottomoduli.\n\n"
                            "Descrizioni dei sottomoduli:\n%s\n"
                            "Fornisci una sintesi chiara, tecnica e professionale dello scopo complessivo del progetto padre in massimo 2-3 frasi. Rispondere solo con la sintesi, senza preamboli.",
                            child_descriptions);
                        printf("--> Generando descrizione sintetizzata per nodo superiore %s...\n", proj->name);
                        generate_text_via_worker(w, prompt, generated, sizeof(generated));
                    }
                    fprintf(fp_out, "%s\n", generated);
                } else {
                    fprintf(fp_out, "%s", desc_accum);
                }
            } else {
                strncat(desc_accum, line, sizeof(desc_accum) - strlen(desc_accum) - 1);
                continue;
            }
        }

        char rel_path[PATH_MAX];
        char desc[2048];
        if (parse_doc_line(line, rel_path, desc)) {
            if (is_placeholder(desc) && strstr(proj->type, "External Library") == NULL) {
                char generated[2048] = {0};
                char prompt[6000];
                char prompt_tmp[6000];
                replace_string(filedesc_template, "[mdfiles]", md_files_str, prompt_tmp, sizeof(prompt_tmp));
                replace_string(prompt_tmp, "[dependency]", dependency_str, prompt, sizeof(prompt));
                replace_string(prompt, "[pathfile]", rel_path, prompt_tmp, sizeof(prompt_tmp));
                printf("--> Generando descrizione per file documentazione %s/%s...\n", proj->name, rel_path);
                generate_text_via_worker(w, prompt_tmp, generated, sizeof(generated));
                int indent = 0;
                while (line[indent] == ' ' || line[indent] == '\t') indent++;
                fprintf(fp_out, "%.*s- [%s](./%s): %s\n", indent, line, rel_path, rel_path, generated);
            } else {
                fprintf(fp_out, "%s", line);
            }
        } else if (parse_src_line(line, rel_path, desc)) {
            if (is_placeholder(desc) && strstr(proj->type, "External Library") == NULL) {
                char generated[2048] = {0};
                char prompt[6000];
                char prompt_tmp[6000];
                replace_string(filedesc_template, "[mdfiles]", md_files_str, prompt_tmp, sizeof(prompt_tmp));
                replace_string(prompt_tmp, "[dependency]", dependency_str, prompt, sizeof(prompt));
                replace_string(prompt, "[pathfile]", rel_path, prompt_tmp, sizeof(prompt_tmp));
                printf("--> Generando descrizione per file sorgente %s/%s...\n", proj->name, rel_path);
                generate_text_via_worker(w, prompt_tmp, generated, sizeof(generated));
                int indent = 0;
                while (line[indent] == ' ' || line[indent] == '\t') indent++;
                fprintf(fp_out, "%.*s- `%s`: %s\n", indent, line, rel_path, generated);
            } else {
                fprintf(fp_out, "%s", line);
            }
        } else {
            fprintf(fp_out, "%s", line);
        }
    }

    if (in_desc_block) {
        if (is_placeholder(desc_accum)) {
            char generated[2048] = {0};
            if (proj->is_leaf) {
                char prompt[4096];
                char rel_md_path[PATH_MAX];
                strcpy(rel_md_path, proj->abs_path);
                int base_len = strlen(base_dir);
                if (strncmp(rel_md_path, base_dir, base_len) == 0) {
                    memmove(rel_md_path, rel_md_path + base_len + 1, strlen(rel_md_path) - base_len);
                }
                replace_string(projdesc_template, "[pathfilemdelprogetto]", rel_md_path, prompt, sizeof(prompt));
                printf("--> Generando descrizione del progetto leaf per %s...\n", proj->name);
                generate_text_via_worker(w, prompt, generated, sizeof(generated));
            } else {
                char child_descriptions[8000] = {0};
                get_descendant_summaries(proj - projects, child_descriptions, sizeof(child_descriptions));
                char prompt[10000];
                snprintf(prompt, sizeof(prompt), 
                    "Sei un analista del software esperto. Il tuo compito è fornire una descrizione generale sintetica per il progetto superiore a partire delle descrizioni dei suoi sottomoduli.\n\n"
                    "Descrizioni dei sottomoduli:\n%s\n"
                    "Fornisci una sintesi chiara, tecnica e professionale dello scopo complessivo del progetto padre in massimo 2-3 frasi. Rispondere solo con la sintesi, senza preamboli.",
                    child_descriptions);
                printf("--> Generando descrizione sintetizzata per nodo superiore %s...\n", proj->name);
                generate_text_via_worker(w, prompt, generated, sizeof(generated));
            }
            fprintf(fp_out, "%s\n", generated);
        } else {
            fprintf(fp_out, "%s", desc_accum);
        }
    }

    fclose(fp_in);
    fclose(fp_out);
    
    unlink(proj->abs_path);
    rename(tmp_path, proj->abs_path);
    return true;
}

/* Helper to get basename of a path */
static const char *get_basename(const char *path) {
    const char *last_sl = strrchr(path, '/');
    return last_sl ? last_sl + 1 : path;
}

/* Helper to check if we should save the KV cache for a project (must not be External Library and must be a Git module/submodule root containing .git) */
static bool should_save_cache(project_info *proj) {
    if (strstr(proj->type, "External Library") != NULL) {
        return false;
    }
    char git_path[PATH_MAX];
    snprintf(git_path, sizeof(git_path), "%s/.git", proj->dir);
    if (access(git_path, F_OK) != 0) {
        return false;
    }
    const char *dir_name = get_basename(proj->dir);
    if (strcmp(proj->name, dir_name) != 0) {
        return false;
    }
    return true;
}

/* Read prompt from generatecache.txt, substitute variable, run LLM and update INDEX.md */
static void generate_project_summary_cache(project_info *proj, agent_worker *w, const char *base_dir) {
    if (w->cfg->dryrun) {
        printf("[DRYRUN] Genero sintesi cache per modulo %s\n", proj->name);
        printf("[DRYRUN] Aggiorno INDEX.md per %s\n", proj->name);
        
        if (should_save_cache(proj)) {
            char kvcache_path[PATH_MAX];
            snprintf(kvcache_path, sizeof(kvcache_path), "%s/kvcache.txt", proj->dir);
            char old_sha[64] = {0};
            FILE *fp_old = fopen(kvcache_path, "r");
            if (fp_old) {
                if (fscanf(fp_old, "%63s", old_sha) == 1) {
                    printf("[DRYRUN] Delete cache %s per %s\n", old_sha, proj->name);
                }
                fclose(fp_old);
            }
            printf("[DRYRUN] Salvo cache per %s\n", proj->name);
        }
        return;
    }

    FILE *fp_md = fopen(proj->abs_path, "r");
    if (!fp_md) return;
    
    fseek(fp_md, 0, SEEK_END);
    long sz = ftell(fp_md);
    fseek(fp_md, 0, SEEK_SET);
    
    char *md_content = xmalloc(sz + 1);
    size_t rd = fread(md_content, 1, sz, fp_md);
    md_content[rd] = '\0';
    fclose(fp_md);

    char cache_template[8000] = {0};
    char path_gc[PATH_MAX];
    snprintf(path_gc, sizeof(path_gc), "%s/generatecache.txt", base_dir);
    FILE *fp_gc = fopen(path_gc, "r");
    if (fp_gc) {
        size_t n = fread(cache_template, 1, sizeof(cache_template) - 1, fp_gc);
        cache_template[n] = '\0';
        fclose(fp_gc);
    }
    if (cache_template[0] == '\0') {
        strcpy(cache_template, 
            "Sei un analista del software esperto. Il tuo compito è fornire una sintesi di massimo 1-2 frasi sul modulo/progetto descritto nel seguente file markdown.\n\n"
            "Contenuto del file markdown del modulo:\n[modulemdfile]\n\n"
            "Generare una descrizione in massimo 1-2 frasi dello scopo del modulo/progetto e del suo ruolo all'interno del sistema complessivo. Sii preciso, tecnico e conciso. Rispondere solo con la descrizione, senza preamboli."
        );
    }

    size_t prompt_max = sz + strlen(cache_template) + 1024;
    char *prompt = xmalloc(prompt_max);
    replace_string(cache_template, "[modulemdfile]", md_content, prompt, prompt_max);
    free(md_content);

    char generated_summary[2048] = {0};
    printf("--> Generando sintesi cache per modulo %s...\n", proj->name);
    generate_text_via_worker(w, prompt, generated_summary, sizeof(generated_summary));
    free(prompt);

    strncpy(proj->desc, generated_summary, sizeof(proj->desc) - 1);
    proj->desc[sizeof(proj->desc) - 1] = '\0';
    
    char new_line[4096];
    char indent_str[128] = {0};
    if (proj->indent < 120) {
        memset(indent_str, ' ', proj->indent);
    }
    snprintf(new_line, sizeof(new_line), "%s- [%s](%s) [%s] - %s\n",
             indent_str, proj->name, proj->rel_path, proj->type, proj->desc);
             
    free(index_lines[proj->line_idx]);
    index_lines[proj->line_idx] = xstrdup(new_line);
    save_index_md_to_disk();

    if (should_save_cache(proj)) {
        char kvcache_path[PATH_MAX];
        snprintf(kvcache_path, sizeof(kvcache_path), "%s/kvcache.txt", proj->dir);
        
        char old_sha[64] = {0};
        FILE *fp_old = fopen(kvcache_path, "r");
        if (fp_old) {
            if (fscanf(fp_old, "%63s", old_sha) == 1) {
                char del_err[160];
                agent_worker_delete_session(w, old_sha, NULL, del_err, sizeof(del_err));
            }
            fclose(fp_old);
        }
        
        pthread_mutex_lock(&w->mu);
        w->user_activity = true;
        w->session_dirty = true;
        pthread_mutex_unlock(&w->mu);
        
        if (!w->session_title) {
            w->session_title = xstrdup("updind-submodule");
        }
        w->session_created_at = (uint64_t)time(NULL);
        
        char new_sha[41];
        int new_tokens = 0;
        char save_err[160];
        if (agent_worker_save_session_now(w, new_sha, &new_tokens, save_err, sizeof(save_err))) {
            printf("Saved submodule KV cache: %s -> %s (%d tokens)\n", proj->name, new_sha, new_tokens);
            FILE *fp_new = fopen(kvcache_path, "w");
            if (fp_new) {
                fprintf(fp_new, "%s\n", new_sha);
                fclose(fp_new);
            }
        } else {
            fprintf(stderr, "ds4-agent: failed to save KV cache for submodule %s: %s\n", proj->name, save_err);
        }
    }
}

/* Command execution function called when running ds4-agent --updind */
static int run_updind(ds4_engine *engine, agent_config *cfg) {
    printf("=== DwarfStar Documentation Indexing Agent (updind) ===\n");
    
    const char *index_path = NULL;
    if (cfg->updind_path) {
        if (access(cfg->updind_path, F_OK) == 0) {
            index_path = cfg->updind_path;
        } else {
            fprintf(stderr, "ds4-agent: il file specificato %s non esiste.\n", cfg->updind_path);
            return 1;
        }
    } else {
        const char *candidates[] = {
            "/Users/simone/aiWorkForce/INDEX.md",
            "./aiWorkForce/INDEX.md",
            "../aiWorkForce/INDEX.md",
            "./INDEX.md"
        };
        for (int i = 0; i < 4; i++) {
            if (access(candidates[i], F_OK) == 0) {
                index_path = candidates[i];
                break;
            }
        }
    }
    
    if (!index_path) {
        fprintf(stderr, "ds4-agent: could not find INDEX.md in workspaces\n");
        return 1;
    }
    
    char abs_index_path[PATH_MAX];
    if (!realpath(index_path, abs_index_path)) {
        fprintf(stderr, "ds4-agent: failed to resolve absolute path for %s\n", index_path);
        return 1;
    }
    printf("Trovata radice albero INDEX.md a: %s\n", abs_index_path);
    strcpy(updind_index_path, abs_index_path);
    
    char base_dir[PATH_MAX];
    strcpy(base_dir, abs_index_path);
    char *last_slash = strrchr(base_dir, '/');
    if (last_slash) {
        *last_slash = '\0';
    } else {
        strcpy(base_dir, ".");
    }
    
    if (chdir(base_dir) != 0) {
        fprintf(stderr, "ds4-agent: failed to chdir to %s: %s\n", base_dir, strerror(errno));
        return 1;
    }

    if (realpath(base_dir, updind_base_dir) == NULL) {
        strncpy(updind_base_dir, base_dir, sizeof(updind_base_dir) - 1);
        updind_base_dir[sizeof(updind_base_dir) - 1] = '\0';
    }
    
    // Load INDEX.md into memory
    FILE *fp_idx = fopen(abs_index_path, "r");
    if (!fp_idx) {
        fprintf(stderr, "ds4-agent: failed to read %s\n", index_path);
        return 1;
    }
    
    char line[4096];
    bool in_moduli_section = false;
    
    while (fgets(line, sizeof(line), fp_idx) && num_index_lines < MAX_INDEX_LINES) {
        index_lines[num_index_lines] = xstrdup(line);
        
        if (strncmp(line, "## Struttura Moduli", 19) == 0) {
            in_moduli_section = true;
            num_index_lines++;
            continue;
        }
        
        if (in_moduli_section) {
            if (line[0] == '#' && line[1] == '#' && line[2] == ' ') {
                in_moduli_section = false;
            } else {
                char name[128], path[PATH_MAX], type[64], desc[2048];
                int indent = 0;
                if (parse_index_line(line, name, path, type, desc, &indent)) {
                    if (num_projects < MAX_PROJECTS) {
                        project_info *proj = &projects[num_projects];
                        strcpy(proj->name, name);
                        strcpy(proj->rel_path, path);
                        
                        const char *p = path;
                        if (p[0] == '.' && p[1] == '/') p += 2;
                        snprintf(proj->abs_path, sizeof(proj->abs_path), "%s/%s", base_dir, p);
                        
                        strcpy(proj->dir, proj->abs_path);
                        char *last_sl = strrchr(proj->dir, '/');
                        if (last_sl) *last_sl = '\0';
                        
                        strcpy(proj->type, type);
                        strcpy(proj->desc, desc);
                        proj->indent = indent;
                        proj->line_idx = num_index_lines;
                        
                        num_projects++;
                    }
                }
            }
        }
        
        num_index_lines++;
    }
    fclose(fp_idx);
    printf("Letti %d progetti dall'albero di documentazione.\n", num_projects);
    
    // Resolve hierarchy
    for (int i = 0; i < num_projects; i++) {
        projects[i].parent_idx = -1;
        projects[i].is_leaf = true;
        projects[i].depth = 0;
    }
    for (int i = 0; i < num_projects; i++) {
        int best_parent = -1;
        int best_len = -1;
        const char *c_dir = projects[i].dir;
        for (int j = 0; j < num_projects; j++) {
            if (i == j) continue;
            const char *p_dir = projects[j].dir;
            int p_len = strlen(p_dir);
            if (strncmp(c_dir, p_dir, p_len) == 0 && c_dir[p_len] == '/') {
                if (p_len > best_len) {
                    best_len = p_len;
                    best_parent = j;
                }
            }
        }
        projects[i].parent_idx = best_parent;
        if (best_parent != -1) {
            projects[best_parent].is_leaf = false;
        }
    }
    
    // Compute depth
    for (int i = 0; i < num_projects; i++) {
        int d = 0;
        int curr = i;
        while (projects[curr].parent_idx != -1) {
            d++;
            curr = projects[curr].parent_idx;
        }
        projects[i].depth = d;
    }
    
    // Sort projects by depth descending (leaf nodes first)
    int proj_order[MAX_PROJECTS];
    for (int i = 0; i < num_projects; i++) proj_order[i] = i;
    for (int i = 0; i < num_projects - 1; i++) {
        for (int j = i + 1; j < num_projects; j++) {
            if (projects[proj_order[i]].depth < projects[proj_order[j]].depth) {
                int tmp = proj_order[i];
                proj_order[i] = proj_order[j];
                proj_order[j] = tmp;
            }
        }
    }
    
    // Initialize agent worker
    agent_worker worker;
    if (agent_worker_init(&worker, engine, cfg) != 0) {
        fprintf(stderr, "ds4-agent: failed to initialize agent worker\n");
        return 1;
    }
    
    // Wait for the background worker thread to finish its startup initialization
    while (true) {
        pthread_mutex_lock(&worker.mu);
        bool ready = worker.initialized;
        pthread_mutex_unlock(&worker.mu);
        if (ready) break;
        usleep(1000);
    }
    
    // Process bottom-up
    for (int o = 0; o < num_projects; o++) {
        int idx = proj_order[o];
        project_info *proj = &projects[idx];
        printf("\n========================================\n");
        printf("[%d/%d] Processando: %s\n", o + 1, num_projects, proj->name);
        printf("Tipo: %s, Directory: %s\n", proj->type, proj->dir);
        printf("Hierarchy Depth: %d, Role: %s\n", proj->depth, proj->is_leaf ? "LEAF" : "PARENT");
        printf("========================================\n");
        
        // Skip missing markdown files
        if (access(proj->abs_path, F_OK) != 0) {
            fprintf(stderr, "Errore: Il file markdown di riferimento non esiste: %s. Salto il modulo %s.\n", proj->abs_path, proj->name);
            continue;
        }
        
        bool updated = process_project_markdown(proj, &worker, base_dir);
        if (updated || is_placeholder(proj->desc)) {
            generate_project_summary_cache(proj, &worker, base_dir);
        }
    }
    
    // Write back INDEX.md
    if (cfg->dryrun) {
        printf("[DRYRUN] Scrivo file INDEX.md a: %s\n", abs_index_path);
    } else {
        save_index_md_to_disk();
    }
    for (int i = 0; i < num_index_lines; i++) {
        free(index_lines[i]);
    }
    updind_index_path[0] = '\0';
    
    agent_worker_free(&worker);
    printf("\n=== Indicizzazione completata con successo! ===\n");
    return 0;
}
