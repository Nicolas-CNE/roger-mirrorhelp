#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <map>
#include <set>
#include <cstdlib>
#include <filesystem>
#include <algorithm>
#include <unistd.h>
#include <sys/wait.h>
#include "RogerSAT.hpp"

namespace fs = std::filesystem;

bool verbose = false;
const std::string REPO_URL = "https://raw.githubusercontent.com/Nicolas-CNE/tango-packages/main/ROGERINDEX";
const std::string INDEX_PATH = "/tmp/ROGERINDEX";
const std::string DB_PATH = "/var/lib/roger/installed/";

void log_verbose(const std::string& msg) {
    if (verbose) std::cout << "[VERBOSE] " << msg << "\n";
}

void ensure_db_dir() {
    if (!fs::exists(DB_PATH)) {
        fs::create_directories(DB_PATH);
    }
}

// Función auxiliar para sanitizar cada nombre de dependencia
std::string sanitize_dep(std::string dep) {
    // Eliminar comillas dobles, simples y comas
    dep.erase(std::remove(dep.begin(), dep.end(), '"'), dep.end());
    dep.erase(std::remove(dep.begin(), dep.end(), '\''), dep.end());
    dep.erase(std::remove(dep.begin(), dep.end(), ','), dep.end());
    // Trim de espacios y caracteres de control
    dep.erase(0, dep.find_first_not_of(" \t\r\n"));
    if (!dep.empty()) {
        dep.erase(dep.find_last_not_of(" \t\r\n") + 1);
    }
    return dep;
}

// Ejecución segura de comandos sin pasar por la Shell
bool exec_safe(const std::vector<std::string>& args) {
    if (args.empty()) return false;
    
    pid_t pid = fork();
    if (pid == 0) {
        std::vector<char*> c_args;
        for (const auto& arg : args) {
            c_args.push_back(const_cast<char*>(arg.c_str()));
        }
        c_args.push_back(nullptr);

        execvp(c_args[0], c_args.data());
        _exit(127);
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
        return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }
    return false;
}

void update_ldconfig() {
    log_verbose("Actualizando la caché de librerías del sistema (ldconfig)...");
    if (!exec_safe({"ldconfig"})) {
        std::cerr << "[WARNING] No se pudo ejecutar ldconfig correctamente.\n";
    }
}

std::map<std::string, PackageSpec> load_installed_packages() {
    std::map<std::string, PackageSpec> installed;
    ensure_db_dir();

    for (const auto& entry : fs::directory_iterator(DB_PATH)) {
        if (entry.path().extension() == ".meta") {
            std::ifstream file(entry.path());
            std::string line;
            PackageSpec pkg;

            while (std::getline(file, line)) {
                size_t colon = line.find(':');
                if (colon != std::string::npos) {
                    std::string key = line.substr(0, colon);
                    std::string val = line.substr(colon + 1);
                    key.erase(0, key.find_first_not_of(" \t"));
                    key.erase(key.find_last_not_of(" \t\r\n") + 1);
                    val.erase(0, val.find_first_not_of(" \t"));
                    val.erase(val.find_last_not_of(" \t\r\n") + 1);

                    if (key == "pkgname") pkg.name = val;
                    else if (key == "version") pkg.version = val;
                    else if (key == "explicit") pkg.explicit_installed = (val == "1");
                    else if (key == "depends") {
                        // Reemplazar comas por espacios para admitir ambos formatos
                        std::replace(val.begin(), val.end(), ',', ' ');
                        std::stringstream ss(val);
                        std::string dep;
                        while (ss >> dep) {
                            dep = sanitize_dep(dep);
                            if (!dep.empty()) pkg.depends.push_back(dep);
                        }
                    }
                }
            }
            if (!pkg.name.empty()) {
                installed[pkg.name] = pkg;
            }
        }
    }
    return installed;
}

DependencyResolverSAT load_index(const std::string& filepath) {
    DependencyResolverSAT sat;
    std::ifstream file(filepath);
    if (!file.is_open()) return sat;

    std::string line;
    PackageSpec current_pkg;
    bool building = false;

    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') {
            if (building && !current_pkg.name.empty()) {
                sat.addPackage(current_pkg);
                current_pkg = PackageSpec();
                building = false;
            }
            continue;
        }

        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            std::string val = line.substr(colon + 1);
            key.erase(0, key.find_first_not_of(" \t"));
            key.erase(key.find_last_not_of(" \t\r\n") + 1);
            val.erase(0, val.find_first_not_of(" \t"));
            val.erase(val.find_last_not_of(" \t\r\n") + 1);

            if (key == "pkgname") {
                if (building && !current_pkg.name.empty()) {
                    sat.addPackage(current_pkg);
                    current_pkg = PackageSpec();
                }
                current_pkg.name = val;
                building = true;
            }
            else if (key == "version") current_pkg.version = val;
            else if (key == "url") current_pkg.url = val;
            else if (key == "sha256") current_pkg.sha256 = val;
            else if (key == "depends") {
                // Reemplazar comas por espacios para admitir listas delimitadas por comas
                std::replace(val.begin(), val.end(), ',', ' ');
                std::stringstream ss(val);
                std::string dep;
                while (ss >> dep) {
                    dep = sanitize_dep(dep);
                    if (!dep.empty()) current_pkg.depends.push_back(dep);
                }
            }
        }
    }

    if (building && !current_pkg.name.empty()) {
        sat.addPackage(current_pkg);
    }

    return sat;
}

void cmd_sync() {
    log_verbose("Descargando ROGERINDEX desde " + REPO_URL);
    if (exec_safe({"curl", "-sL", REPO_URL, "-o", INDEX_PATH})) {
        std::cout << "Sincronización completa. Índice guardado en " << INDEX_PATH << "\n";
    } else {
        std::cerr << "[ERROR] Falló la sincronización del repositorio.\n";
    }
}

bool verify_checksum(const std::string& filepath, const std::string& expected_sha) {
    if (expected_sha.empty()) return true;
    std::string check_spec = expected_sha + "  " + filepath;
    
    int pipefd[2];
    if (pipe(pipefd) == -1) return false;

    pid_t pid = fork();
    if (pid == 0) {
        dup2(pipefd[0], STDIN_FILENO);
        close(pipefd[1]);
        close(pipefd[0]);
        execlp("sha256sum", "sha256sum", "-c", "--status", nullptr);
        _exit(127);
    }

    close(pipefd[0]);
    write(pipefd[1], check_spec.c_str(), check_spec.size());
    close(pipefd[1]);

    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

void cmd_install(const std::string& pkg) {
    DependencyResolverSAT sat = load_index(INDEX_PATH);
    std::vector<std::string> to_install;

    log_verbose("Llamando a MiniSAT para resolver dependencias de: " + pkg);
    if (!sat.resolveInstall(pkg, to_install)) {
        std::cout << "[ERROR] No se pudo resolver la instalación (UNSATISFIABLE o dependencias faltantes).\n";
        return;
    }

    ensure_db_dir();
    std::vector<std::string> pending_execution;

    for (const auto& p : to_install) {
        std::string meta_path = DB_PATH + p + ".meta";
        if (fs::exists(meta_path) && p != pkg) {
            log_verbose("La dependencia '" + p + "' ya está instalada. Omitiendo.");
            continue;
        }
        pending_execution.push_back(p);
    }

    if (pending_execution.empty()) {
        std::cout << "El paquete '" << pkg << "' y sus dependencias ya están instalados.\n";
        return;
    }

    std::cout << "Plan de instalación (" << pending_execution.size() << " paquetes):\n";
    for (const auto& p : pending_execution) {
        std::cout << "  - " << p << "\n";
    }

    // FASE 1: DESCARGA EN LOTE (FETCH-FIRST)
    std::cout << "\n[1/2] Descargando paquetes...\n";
    for (const auto& p : pending_execution) {
        PackageSpec spec = sat.getPackageSpec(p);
        std::string archive_name = "/tmp/" + p + ".tango.tar.zst";

        std::cout << "  Downloading " << p << "...\n";
        if (!exec_safe({"curl", "-f", "-sL", spec.url, "-o", archive_name})) {
            std::cerr << "[ERROR CRÍTICO] Falló la descarga de " << p << ". Abortando transacción sin modificar el sistema.\n";
            return;
        }

        if (!verify_checksum(archive_name, spec.sha256)) {
            std::cerr << "[ERROR CRÍTICO] Checksum SHA256 inválido para " << p << ". Abortando instalación.\n";
            fs::remove(archive_name);
            return;
        }
    }

    // FASE 2: EXTRACCIÓN Y REGISTRO
    std::cout << "\n[2/2] Instalando en el sistema raíz...\n";
    bool installed_any = false;

    for (const auto& p : pending_execution) {
        PackageSpec spec = sat.getPackageSpec(p);
        std::string archive_name = "/tmp/" + p + ".tango.tar.zst";
        std::string meta_path = DB_PATH + p + ".meta";
        std::string file_list = DB_PATH + p + ".files";

        int out_fd = fileno(fopen(file_list.c_str(), "w"));
        if (out_fd != -1) {
            pid_t pid = fork();
            if (pid == 0) {
                dup2(out_fd, STDOUT_FILENO);
                execlp("tar", "tar", "-I", "zstd", "-tf", archive_name.c_str(), nullptr);
                _exit(127);
            }
            close(out_fd);
            int status;
            waitpid(pid, &status, 0);

            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                std::cerr << "[ERROR] Falló la indexación de " << p << "\n";
                fs::remove(archive_name);
                fs::remove(file_list);
                continue;
            }
        }

        if (exec_safe({"tar", "-I", "zstd", "-Pxf", archive_name, "-C", "/"})) {
            std::cout << "  -> " << p << " instalado correctamente.\n";
            installed_any = true;

            std::ofstream meta(meta_path);
            meta << "pkgname: " << p << "\n";
            meta << "version: " << spec.version << "\n";
            meta << "explicit: " << (p == pkg ? "1" : "0") << "\n";
            meta << "depends: ";
            for (size_t i = 0; i < spec.depends.size(); ++i) {
                meta << spec.depends[i] << (i + 1 < spec.depends.size() ? " " : "");
            }
            meta << "\n";
            meta.close();
        } else {
            std::cerr << "[ERROR] Falló la extracción de " << p << "\n";
            fs::remove(file_list);
        }

        fs::remove(archive_name);
    }

    if (installed_any) {
        update_ldconfig();
        std::cout << "\n¡Proceso de instalación completado con éxito!\n";
    }
}

void cmd_delete(const std::string& pkg) {
    ensure_db_dir();
    DependencyResolverSAT sat;
    std::string files_path = DB_PATH + pkg + ".files";
    std::string meta_path = DB_PATH + pkg + ".meta";

    if (sat.isProtected(pkg)) {
        std::cerr << "[BLINDAJE DE SEGURIDAD] El paquete '" << pkg << "' pertenece a la base crítica del sistema. No se puede eliminar.\n";
        return;
    }

    if (!fs::exists(meta_path)) {
        std::cerr << "[ERROR] El paquete '" << pkg << "' no está instalado en Tango Linux.\n";
        return;
    }

    log_verbose("Eliminando archivos de " + pkg);
    std::ifstream files_file(files_path);
    std::string file_to_remove;
    std::set<fs::path> candidate_dirs;

    while (std::getline(files_file, file_to_remove)) {
        if (file_to_remove.empty()) continue;
        std::string full_path = (file_to_remove[0] == '/') ? file_to_remove : "/" + file_to_remove;
        fs::path p(full_path);

        std::error_code ec;
        auto status = fs::symlink_status(p, ec);
        if (!ec && fs::exists(status) && !fs::is_directory(status)) {
            fs::remove(p, ec);
            if (!ec) {
                candidate_dirs.insert(p.parent_path());
            }
        }
    }

    for (const auto& dir : candidate_dirs) {
        std::error_code ec;
        if (fs::exists(dir) && fs::is_empty(dir, ec)) {
            log_verbose("Removiendo directorio vacío: " + dir.string());
            fs::remove_all(dir, ec);
        }
    }

    fs::remove(files_path);
    fs::remove(meta_path);
    update_ldconfig();

    std::cout << "Paquete '" << pkg << "' eliminado exitosamente.\n";
}

void cmd_bomb() {
    log_verbose("Iniciando escaneo de huérfanos (roger bomb)...");
    auto installed = load_installed_packages();
    DependencyResolverSAT sat;

    auto orphans = sat.findOrphans(installed);

    if (orphans.empty()) {
        std::cout << "[roger bomb] No se encontraron dependencias huérfanas en el sistema.\n";
        return;
    }

    std::cout << "[roger bomb] Se encontraron las siguientes dependencias huérfanas:\n";
    for (const auto& orphan : orphans) {
        std::cout << "  - " << orphan << "\n";
    }

    std::cout << "Eliminando dependencias huérfanas...\n";
    for (const auto& orphan : orphans) {
        cmd_delete(orphan);
    }
    std::cout << "[roger bomb] Limpieza completada con éxito.\n";
}

void cmd_update() {
    cmd_sync();
    auto installed = load_installed_packages();
    DependencyResolverSAT sat = load_index(INDEX_PATH);

    std::cout << "\nComprobando actualizaciones de paquetes...\n";
    std::vector<std::string> to_upgrade;

    for (const auto& [name, spec] : installed) {
        PackageSpec repo_spec = sat.getPackageSpec(name);
        if (!repo_spec.name.empty() && repo_spec.version != spec.version) {
            std::cout << "  - " << name << " (" << spec.version << " -> " << repo_spec.version << ")\n";
            to_upgrade.push_back(name);
        }
    }

    if (to_upgrade.empty()) {
        std::cout << "Todos los paquetes instalados están actualizados a la última versión.\n";
        return;
    }

    std::cout << "\nActualizando paquetes requeridos...\n";
    for (const auto& pkg : to_upgrade) {
        cmd_install(pkg);
    }
}

int main(int argc, char* argv[]) {
    int arg_start = 1;
    if (argc > 1 && std::string(argv[1]) == "--verbose") {
        verbose = true;
        arg_start = 2;
    }

    if (arg_start >= argc) {
        std::cout << "Uso: roger [--verbose] <sync|install|delete|bomb|update> [paquete]\n";
        return 1;
    }

    std::string command = argv[arg_start];

    if (command == "sync") cmd_sync();
    else if (command == "install" && arg_start + 1 < argc) cmd_install(argv[arg_start + 1]);
    else if (command == "delete" && arg_start + 1 < argc) cmd_delete(argv[arg_start + 1]);
    else if (command == "bomb") cmd_bomb();
    else if (command == "update") cmd_update();
    else std::cout << "Comando desconocido: " << command << "\n";

    return 0;
}
