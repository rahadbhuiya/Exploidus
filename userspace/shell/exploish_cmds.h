#pragma once
void cmd_ext_pwd(void);
void cmd_ext_cd(const char *path);
void cmd_ext_whoami(void);
void cmd_ext_hostname(void);
void cmd_ext_mkdir(const char *path);
void cmd_ext_rm(const char *path);
void cmd_ext_free(void);
void cmd_ext_uptime(void);
void cmd_ext_kill(const char *args);
void cmd_ext_ifconfig(void);
void cmd_ext_wc(const char *path);
void cmd_ext_grep(const char *args);
void cmd_ext_head(const char *args);
void cmd_ext_xxd(const char *path);
