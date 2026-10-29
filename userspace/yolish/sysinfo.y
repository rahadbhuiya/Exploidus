-- sysinfo.y
-- Exploidus OS Native System Information Utility in Yolish

fn print_header() {
    y.println("==================================================")
    y.println("        EXPLOIDUS REACTIVE CAPABILITY OS          ")
    y.println("==================================================")
}

fn print_system_details() {
    y.println("  OS Name       : Exploidus OS")
    y.println("  Kernel Arch   : x86_64 (From Scratch)")
    y.println("  Release       : v0.1.0-alpha")
    y.println("  Native Shell  : exploish")
    y.println("  Native Ext    : .fozu")
    y.println("  Package Mgr   : rahu")
    y.println("  Native Lang   : Yolish v2.48")
    y.println("  Default User  : star@exploidus")
}

fn check_capabilities() {
    y.println("--------------------------------------------------")
    y.println("[CAPABILITY AUDIT]")
    
    let log_cap = cap.open("/var/log", 1)
    if cap.perm(log_cap) > 0 {
        y.println("  [/var/log]     : READ PERMISSION GRANTED")
        cap.close(log_cap)
    } else {
        y.println("  [/var/log]     : RESTRICTED")
    }

    let bin_cap = cap.open("/bin", 1)
    if cap.perm(bin_cap) > 0 {
        y.println("  [/bin]         : READ PERMISSION GRANTED")
        cap.close(bin_cap)
    } else {
        y.println("  [/bin]         : RESTRICTED")
    }
}

fn main() {
    print_header()
    print_system_details()
    check_capabilities()
    y.println("==================================================")
}
