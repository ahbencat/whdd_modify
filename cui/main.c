#define _GNU_SOURCE

#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <wchar.h>
#include <locale.h>
#include <errno.h>
#include <unistd.h>
#include <curses.h>
#include <dialog.h>
#include "libdevcheck.h"
#include "device.h"
#include "utils.h"
#include "procedure.h"
#include "vis.h"
#include "ncurses_convenience.h"
#include "render.h"
#include "ui_mutual.h"

static int global_init(void);
static void global_fini(void);
static DC_Dev *menu_choose_device(DC_DevList *devlist);
static DC_Procedure *menu_choose_procedure(DC_Dev *dev);
void log_cb(void *priv, enum DC_LogLevel level, const char* fmt, va_list vl);

static int ask_option_value(DC_Dev *dev, DC_Procedure *act, DC_OptionSetting *setting, DC_ProcedureOption *option) {
    int r;
    char *suggested_value = setting->value;
    char entered_value[200];
    const char *param_type_str;
    char *config_supplied_value;
    char *config_suggested_value;
    char config_search_command[200];
    snprintf(config_search_command, sizeof(config_search_command),
            "grep ^%s.%s= ~/.whddrc 2>/dev/null | awk -F= '{print $2}' | tr -d '\\n'", act->name, option->name);
    config_supplied_value = cmd_output(config_search_command);
    if (config_supplied_value) {
        setting->value = config_supplied_value;
        return 0;
    }

    snprintf(config_search_command, sizeof(config_search_command),
            "grep ^%s.%s.suggest= ~/.whddrc 2>/dev/null | awk -F= '{print $2}' | tr -d '\\n'", act->name, option->name);
    config_suggested_value = cmd_output(config_search_command);
    if (config_suggested_value)
        suggested_value = config_suggested_value;

    switch (option->type) {
        case DC_ProcedureOptionType_eInt64:
            param_type_str = "numeric";
            break;
        case DC_ProcedureOptionType_eString:
            param_type_str = "string";
            break;
    }
    char prompt[500];
    snprintf(prompt, sizeof(prompt), "Please enter %s parameter: %s (%s)",
            param_type_str, option->name, option->help);
    if (option->choices)
        snprintf(prompt + strlen(prompt), sizeof(prompt) - strlen(prompt),
                "\nPress Space to select a choice, then Enter to confirm.");

    dialog_vars.default_button = -1;  // Workaround for surprisingly unfocused input field on old libdialog
    dialog_vars.input_result = NULL;

    if (option->choices) {
        int nb_choices = 0;
        // A quasi-table form which dialog(3) accepts:
        // dialog_checklist... items: is an array of strings which is viewed... as a list of rows `tag` `item` `status`
        const char **choices_for_dialog = NULL;
        //for (const char * choice = option->choices[0]; choice; choice++) {
        const char * choice;
        int items_table_cols = 2;
        assert(dialog_vars.no_items == true); // otherwise one more quasi-column "item"
        assert(dialog_vars.item_help == false); // otherwise one more quasi-column "help" which goes last
        int src_i;
        for (src_i = 0; choice = option->choices[src_i]; src_i++) {
            // Don't offer "ata" API choice on non-ATA-capable devices
            if (!strcmp(choice, "ata") && !dev->ata_capable)
                continue;
            choices_for_dialog = reallocarray(choices_for_dialog, items_table_cols * (nb_choices + 1), sizeof(char*));
            choices_for_dialog[items_table_cols * nb_choices + 0] = choice;
            choices_for_dialog[items_table_cols * nb_choices + 1] = !strcmp(choice, suggested_value)  ? "on" : "off";
            nb_choices++;
        }
        r = dialog_checklist("Input box", prompt, /*height*/0, /*width*/0, /*list_height*/0, nb_choices, /*char **items*/(char **)choices_for_dialog, /*flag*/FLAG_RADIO);
    } else {
        r = dialog_inputbox("Input box", prompt, 0, 0, suggested_value, 0);
    }
    if (r != 0) {
        dialog_msgbox("Info", "Action cancelled", 0, 0, 1);
        return 1;
    }
    // Wow, libdialog is awesomely sane and brilliantly documented lib, i fuckin love it
    snprintf(entered_value, sizeof(entered_value), "%s", dialog_vars.input_result);
    if (entered_value[0] == '\0' || entered_value[0] == '\n')
        snprintf(entered_value, sizeof(entered_value), "%s", suggested_value);
    setting->value = strdup(entered_value);
    free(suggested_value);
    return 0;
}

#include "version.h"

static void print_version(void) {
    printf("whdd %s\n", WHDD_VERSION);
}

static void print_usage(void) {
    printf("Usage: whdd [OPTION]\n");
    printf("HDD diagnostic tool (ncurses UI)\n\n");
    printf("  --version                    print version and exit\n");
    printf("  --list-devices               list all block devices and exit\n");
    printf("  --quick-diagnosis <device>   quick diagnosis on device and exit\n");
    printf("  --smart <device>             show SMART attributes and exit\n");
    printf("  --help                       print this help and exit\n\n");
    printf("Note: some operations (quick-diagnosis, smart) may require sudo/root\n");
    printf("permissions to access block devices.\n");
}

static int list_devices(void) {
    int r;
    r = dc_init();
    if (r) {
        fprintf(stderr, "libdevcheck init fail\n");
        return r;
    }
    dc_log_set_callback(NULL, NULL);
    DC_DevList *devlist = dc_dev_list();
    if (!devlist) {
        fprintf(stderr, "Cannot get device list\n");
        return 1;
    }
    int n = dc_dev_list_size(devlist);
    if (n == 0) {
        printf("No devices found\n");
        return 0;
    }
    for (int i = 0; i < n; i++) {
        DC_Dev *dev = dc_dev_list_get_entry(devlist, i);
        printf("%s  %s  %s  %" PRIu64 " bytes  %s\n",
               dev->dev_fs_name,
               dev->model_str ? dev->model_str : "unknown model",
               dev->serial_no ? dev->serial_no : "unknown serial",
               dev->capacity,
               dev->mounted ? "mounted" : "unmounted");
    }
    dc_dev_list_free(devlist);
    return 0;
}

static DC_Dev *find_device_by_name(const char *name) {
    DC_DevList *devlist = dc_dev_list();
    if (!devlist)
        return NULL;
    int n = dc_dev_list_size(devlist);
    for (int i = 0; i < n; i++) {
        DC_Dev *dev = dc_dev_list_get_entry(devlist, i);
        if (!strcmp(dev->dev_fs_name, name) || !strcmp(dev->dev_path, name))
            return dev;  /* devlist intentionally not freed; CLI is short-lived */
    }
    return NULL;
}

static int run_quick_diagnosis(const char *dev_name) {
    int r;
    r = dc_init();
    if (r) {
        fprintf(stderr, "libdevcheck init fail\n");
        return r;
    }
    dc_log_set_callback(dc_log_default_func, NULL);
    DC_Dev *dev = find_device_by_name(dev_name);
    if (!dev) {
        fprintf(stderr, "Device '%s' not found\n", dev_name);
        return 1;
    }
    fprintf(stderr, "Quick diagnosis on %s (%s):\n", dev->dev_fs_name,
            dev->model_str ? dev->model_str : "unknown model");
    DC_Procedure *proc = NULL;
    while ((proc = dc_get_next_procedure(proc))) {
        if (!strcmp(proc->name, "quick_diagnosis"))
            break;
    }
    if (!proc) {
        fprintf(stderr, "quick_diagnosis procedure not found\n");
        return 1;
    }
    DC_OptionSetting *options = calloc(1, sizeof(DC_OptionSetting));
    DC_ProcedureCtx *ctx = NULL;
    r = dc_procedure_open(proc, dev, &ctx, options);
    if (r)
        fprintf(stderr, "Procedure open failed: %d\n", r);
    else
        dc_procedure_close(ctx);
    free(options);
    return r;
}

static int run_smart(const char *dev_name) {
    int r;
    r = dc_init();
    if (r) {
        fprintf(stderr, "libdevcheck init fail\n");
        return r;
    }
    dc_log_set_callback(dc_log_default_func, NULL);
    DC_Dev *dev = find_device_by_name(dev_name);
    if (!dev) {
        fprintf(stderr, "Device '%s' not found\n", dev_name);
        return 1;
    }
    DC_Procedure *proc = NULL;
    while ((proc = dc_get_next_procedure(proc))) {
        if (!strcmp(proc->name, "smart_show"))
            break;
    }
    if (!proc) {
        fprintf(stderr, "smart_show procedure not found\n");
        return 1;
    }
    DC_OptionSetting *options = calloc(1, sizeof(DC_OptionSetting));
    DC_ProcedureCtx *ctx = NULL;
    r = dc_procedure_open(proc, dev, &ctx, options);
    if (r)
        fprintf(stderr, "Procedure open failed: %d\n", r);
    else
        dc_procedure_close(ctx);
    free(options);
    return r;
}

int main(int argc, char **argv) {
    int r;
    int i;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--version")) {
            print_version();
            return 0;
        }
        if (!strcmp(argv[i], "--list-devices")) {
            return list_devices();
        }
        if (!strcmp(argv[i], "--quick-diagnosis")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --quick-diagnosis requires a device name\n");
                return 1;
            }
            return run_quick_diagnosis(argv[++i]);
        }
        if (!strcmp(argv[i], "--smart")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --smart requires a device name\n");
                return 1;
            }
            return run_smart(argv[++i]);
        }
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            print_usage();
            return 0;
        }
        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        print_usage();
        return 1;
    }
    r = global_init();
    if (r) {
        fprintf(stderr, "init fail\n");
        return r;
    }
    // get list of devices
    DC_DevList *devlist = dc_dev_list();
    assert(devlist);

    while (1) {
        // draw menu of device choice
        DC_Dev *chosen_dev = menu_choose_device(devlist);
        if (!chosen_dev) {
            break;
        }
        // draw procedures menu
        DC_Procedure *act = menu_choose_procedure(chosen_dev);
        if (!act)
            continue;
        if (act->flags & DC_PROC_FLAG_INVASIVE) {
            char *ask;
            r = asprintf(&ask, "This operation is invasive, i.e. it may make your data unreachable or even destroy it completely. Are you sure you want to proceed it on %s (%s)?",
                    chosen_dev->dev_fs_name, chosen_dev->model_str);
            assert(r != -1);
            dialog_vars.default_button = 1;  // Focus on "No"
            r = dialog_yesno("Confirmation", ask, 0, 0);
            // Yes = 0 (FALSE), No = 1, Escape = -1
            free(ask);
            if (/* No */ r)
                continue;
            if (chosen_dev->mounted) {
                dialog_vars.default_button = 1;  // Focus on "No"
                r = dialog_yesno("Confirmation", "This disk is mounted. Are you really sure you want to proceed?", 0, 0);
                if (r)
                    continue;
            }
        }
        DC_OptionSetting *option_set = calloc(act->options_num + 1, sizeof(DC_OptionSetting));
        int i;
        r = 0;
        for (i = 0; i < act->options_num; i++) {
            option_set[i].name = act->options[i].name;
            r = act->suggest_default_value(chosen_dev, &option_set[i]);
            if (r) {
                dc_log(DC_LOG_ERROR, "Failed to get default value suggestion on '%s'", option_set[i].name);
                break;
            }
            r = ask_option_value(chosen_dev, act, &option_set[i], &act->options[i]);
            if (r)
                break;
        }
        if (r)
            continue;
        // Show relaxing banner when copying with journal. Copy journal processing takes some time.
        if (!strcmp(act->name, "copy")) {
            int uses_journal = 0;
            for (i = 0; i < act->options_num; i++) {
                if (!strcmp(option_set[i].name, "use_journal")) {
                    uses_journal = 1;
                    break;
                }
            }
            if (uses_journal)
                dialog_msgbox("Info", "Please wait while operation journal is processed", 0, 0, 0 /* non-pausing */);
        }

        clear_body();

        DC_ProcedureCtx *actctx;
        r = dc_procedure_open(act, chosen_dev, &actctx, option_set);
        if (r) {
            dialog_msgbox("Error", "Procedure init fail", 0, 0, 1);
            continue;
        }
        if (!act->perform) {
            dc_procedure_close(actctx);
            continue;
        }
        DC_Renderer *renderer;
        if (!strcmp(act->name, "copy"))
            renderer = dc_find_renderer("whole_space");
        else
            renderer = dc_find_renderer("sliding_window");
        // Must run before renderer Open() draws legends from bs_vis[]
        vis_set_block_size(actctx->blk_size / 512);
        render_procedure(actctx, renderer);
    } // while(1)

    return 0;
}

static int global_init(void) {
    int r;
    // TODO check all retcodes
    setlocale(LC_ALL, "");
    initscr();
    init_dialog(stdin, stdout);
    dialog_vars.item_help = 0;

    start_color();
    init_my_colors();
    noecho();
    cbreak();
    scrollok(stdscr, FALSE);
    keypad(stdscr, TRUE);

    clear_body();
    // init libdevcheck
    r = dc_init();
    assert(!r);
    RENDERER_REGISTER(sliding_window);
    RENDERER_REGISTER(whole_space);
    dc_log_set_callback(log_cb, NULL);
    r = atexit(global_fini);
    assert(r == 0);
    return 0;
}

static void global_fini(void) {
    clear();
    endwin();
}

static DC_Dev *menu_choose_device(DC_DevList *devlist) {
    int devs_num = dc_dev_list_size(devlist);
    if (devs_num == 0) {
        dialog_msgbox("Info", "No devices found", 0, 0, 1);
        return NULL;
    }
    char *items[2 * devs_num];
    int i;
    for (i = 0; i < devs_num; i++) {
        DC_Dev *dev = dc_dev_list_get_entry(devlist, i);
        char dev_descr_buf[80];
        ui_dev_descr_format(dev_descr_buf, sizeof(dev_descr_buf), dev);
        items[2*i] = dev->dev_fs_name;
        items[2*i+1] = strdup(dev_descr_buf);
    }

    clear_body();
    dialog_vars.no_items = 0;
    dialog_vars.item_help = 0;
    dialog_vars.input_result = NULL;
    dialog_vars.default_button = 0;  // Focus on "OK"
    int ret = dialog_menu("Choose device", "", 0, 0, 0, devs_num, items);
    for (i = 0; i < devs_num; i++)
        free(items[2*i+1]);

    if (ret != 0)
        return NULL;
    for (i = 0; i < devs_num; i++) {
        DC_Dev *dev = dc_dev_list_get_entry(devlist, i);
        if (!strcmp(dev->dev_fs_name, dialog_vars.input_result))
            return dev;
    }
    assert(0);
    return NULL;
}

static DC_Procedure *menu_choose_procedure(DC_Dev *dev) {
    (void)dev;
    int nb_procedures = dc_get_nb_procedures();
    const char *items[nb_procedures];
    DC_Procedure *procedures[nb_procedures];
    int nb_items = 0;
    DC_Procedure *procedure = NULL;
    while ((procedure = dc_get_next_procedure(procedure))) {
        if (!dev->ata_capable && (procedure->flags & DC_PROC_FLAG_REQUIRES_ATA))
            continue;
        items[nb_items] = procedure->display_name;
        procedures[nb_items] = procedure;
        nb_items++;
    }

    while (1) {
        clear_body();
        dialog_vars.no_items = 1;
        dialog_vars.item_help = 0;
        dialog_vars.input_result = NULL;
        dialog_vars.default_button = 0;  // Focus on "OK"
        dialog_vars.extra_button = 1;
        dialog_vars.extra_label = "Help";
        int ret = dialog_menu("Choose procedure", "", 0, 0, 0, nb_items, (/* should be const */char**)items);
        dialog_vars.extra_button = 0;
        if ((ret != DLG_EXIT_OK) && (ret != DLG_EXIT_EXTRA))
            return NULL;  // User quit dialog, exit

        procedure = NULL;
        for (int i = 0; i < nb_items; i++)
            if (!strcmp(items[i], dialog_vars.input_result)) {
                procedure = procedures[i];
                break;
            }
        assert(procedure);

        if (ret == DLG_EXIT_EXTRA) {
            dialog_msgbox(procedure->display_name, procedure->help ? : "No help", 0, 0, 1);
            continue;
        }
        assert(ret == DLG_EXIT_OK);
        return procedure;
    }
    assert(0);  // Never reached
    return NULL;
}

void log_cb(void *priv, enum DC_LogLevel level, const char* fmt, va_list vl) {
    (void)priv;
    char *msg = dc_log_default_form_string(level, fmt, vl);
    assert(msg);
    dialog_msgbox(log_level_name(level), msg, 0, 0, 1);
    free(msg);
}
