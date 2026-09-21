#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <sstream>
#include <cstdlib>
#include <sys/stat.h>
#include <curl/curl.h>
#include <regex>
#include <iomanip>

// Importar interfaz o cabecera SAT de Roger si se compila en conjunto
#include "RogerSAT.hpp"

struct MirrorConfig {
    std::string tango_repo;
    std::vector<std::string> arch_mirrors;
};

struct PackageInfo {
    std::string name;
    std::string version;
    std::string filename;
    long download_bytes = 0;
    long installed_bytes = 0;
    std::vector<std::string> dependencies;
};

// Función auxiliar para verificar si existe un archivo
bool file_exists(const std::string& path) {
    struct stat buffer;
    return (stat(path.c_str(), &buffer) == 0);
}

// Callback para escritura de CURL
size_t write_data(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    return fwrite(ptr, size, nmemb, stream);
}

// Cargar /etc/roger/tmirrors.tango con formato MIRRORS=("..."), ("...")
MirrorConfig load_config(const std::string& config_path) {
    MirrorConfig cfg;
    std::ifstream file(config_path);
    
    if (!file.is_open()) {
        std::cerr << "[!] No se pudo abrir " << config_path << ". Usando mirrors por defecto." << std::endl;
        cfg.arch_mirrors.push_back("https://geo.mirror.pkgbuild.com/core/os/x86_64/");
        cfg.arch_mirrors.push_back("https://geo.mirror.pkgbuild.com/extra/os/x86_64/");
        return cfg;
    }

    std::string line;
    std::regex quote_regex("\"([^\"]+)\"");

    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;

        size_t eq_pos = line.find('=');
        if (eq_pos != std::string::npos) {
            std::string key = line.substr(0, eq_pos);

            if (key == "TANGO_REPO") {
                cfg.tango_repo = line.substr(eq_pos + 1);
            } else if (key == "MIRRORS") {
                auto words_begin = std::sregex_iterator(line.begin(), line.end(), quote_regex);
                auto words_end = std::sregex_iterator();

                for (std::sregex_iterator i = words_begin; i != words_end; ++i) {
                    std::smatch match = *i;
                    cfg.arch_mirrors.push_back(match[1].str());
                }
            }
        }
    }

    file.close();

    if (cfg.arch_mirrors.empty()) {
        cfg.arch_mirrors.push_back("https://geo.mirror.pkgbuild.com/core/os/x86_64/");
    }

    return cfg;
}

// Formatear bytes a MB o GB de forma legible
std::string format_size(long bytes) {
    double size = static_cast<double>(bytes);
    std::ostringstream out;
    out << std::fixed << std::setprecision(2);

    if (size >= 1024 * 1024 * 1024) {
        out << (size / (1024 * 1024 * 1024)) << " GB";
    } else {
        out << (size / (1024 * 1024)) << " MB";
    }
    return out.str();
}

// Descarga con validación HTTP 200
bool download_file(const std::string& url, const std::string& outfilename) {
    CURL *curl = curl_easy_init();
    if (!curl) return false;

    FILE *fp = fopen(outfilename.c_str(), "wb");
    if (!fp) {
        curl_easy_cleanup(curl);
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_data);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "TangoLinux-MirrorHelp/1.0");

    CURLcode res = curl_easy_perform(curl);
    
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    fclose(fp);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || http_code != 200) {
        remove(outfilename.c_str());
        return false;
    }

    return true;
}

// Obtener detalles del paquete desde la API de Arch
bool query_package_info(const std::string& pkg_name, PackageInfo& info) {
    std::string api_url = "https://archlinux.org/packages/search/json/?name=" + pkg_name;
    
    std::string cmd_filename = "curl -s '" + api_url + "' | grep -o '\"filename\": \"[^\"]*\"' | head -n 1 | cut -d'\"' -f4";
    std::string cmd_compressed = "curl -s '" + api_url + "' | grep -o '\"compressed_size\": [0-9]*' | head -n 1 | awk '{print $2}'";
    std::string cmd_installed = "curl -s '" + api_url + "' | grep -o '\"installed_size\": [0-9]*' | head -n 1 | awk '{print $2}'";

    auto exec_cmd = [](const std::string& cmd) -> std::string {
        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) return "";
        char buffer[128];
        std::string result = "";
        while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
            result += buffer;
        }
        pclose(pipe);
        if (!result.empty() && result.back() == '\n') result.pop_back();
        return result;
    };

    info.name = pkg_name;
    info.filename = exec_cmd(cmd_filename);

    if (info.filename.empty()) return false;

    std::string c_size = exec_cmd(cmd_compressed);
    std::string i_size = exec_cmd(cmd_installed);

    info.download_bytes = c_size.empty() ? 5000000 : std::atol(c_size.c_str());
    info.installed_bytes = i_size.empty() ? info.download_bytes * 4 : std::atol(i_size.c_str());

    info.dependencies = {"glibc", "zstd"};

    return true;
}

// Prompt interactivo requerido
bool confirm_prompt(const std::vector<PackageInfo>& pkgs) {
    std::string pkgs_str = "";
    std::string deps_str = "";
    long total_download = 0;
    long total_installed = 0;

    for (const auto& pkg : pkgs) {
        pkgs_str += pkg.name + " ";
        total_download += pkg.download_bytes;
        total_installed += pkg.installed_bytes;
        for (const auto& dep : pkg.dependencies) {
            if (deps_str.find(dep) == std::string::npos) {
                deps_str += dep + " ";
            }
        }
    }

    std::cout << "\nPaquete/s a instalar: " << pkgs_str << "\n";
    std::cout << "Dependencia/s: " << (deps_str.empty() ? "Ninguna" : deps_str) << "\n\n";
    std::cout << "tamaño a tomar en disco: " << format_size(total_installed) << "\n";
    std::cout << "tamaño a descargar e instalar: " << format_size(total_download) << "\n\n";

    std::cout << "estas seguro [y,n]? ";
    char choice;
    std::cin >> choice;

    return (choice == 'y' || choice == 'Y');
}

// Selección interactiva de mirror
std::string select_mirror_dialog(const std::vector<std::string>& mirrors) {
    std::cout << "\n--- Selección de Mirror para Tango Linux ---\n";
    for (size_t i = 0; i < mirrors.size(); ++i) {
        std::cout << "[" << i + 1 << "] " << mirrors[i] << "\n";
    }
    std::cout << "Selecciona el mirror a utilizar (1-" << mirrors.size() << "): ";

    size_t choice = 1;
    std::cin >> choice;

    if (choice < 1 || choice > mirrors.size()) {
        std::cout << "[!] Opción inválida. Usando el mirror por defecto (1).\n";
        return mirrors[0];
    }

    return mirrors[choice - 1];
}

// Conversión dinámicamente limpia de .pkg.tar.zst a .tango.tar.zst
bool convert_arch_to_tango(const std::string& pkg_path, const std::string& out_tango_path) {
    std::cout << "[*] Convirtiendo paquete Arch a formato Tango Linux..." << std::endl;
    
    std::string tmp_dir = "/tmp/tango_convert_" + std::to_string(rand());
    system(("mkdir -p " + tmp_dir).c_str());

    int res1 = system(("tar -I zstd -xf " + pkg_path + " -C " + tmp_dir + " > /dev/null 2>&1").c_str());
    if (res1 != 0) {
        std::cerr << "[!] Error al extraer el paquete comprimido de Arch." << std::endl;
        system(("rm -rf " + tmp_dir + " " + pkg_path).c_str());
        return false;
    }

    // Limpiar metadatos de Arch
    system(("rm -rf " + tmp_dir + "/.PKGINFO " + tmp_dir + "/.BUILDINFO " + tmp_dir + "/.MTREE").c_str());

    int res2 = system(("tar -I 'zstd -19' -cf " + out_tango_path + " -C " + tmp_dir + " . > /dev/null 2>&1").c_str());

    system(("rm -rf " + tmp_dir + " " + pkg_path).c_str());

    return (res2 == 0);
}

// Instalación directa a la raíz del sistema
bool install_tango_pkg(const std::string& tango_pkg_path) {
    std::cout << "[*] Instalando " << tango_pkg_path << " en el sistema..." << std::endl;
    int res = system(("tar -I zstd -xf " + tango_pkg_path + " -C /").c_str());
    return (res == 0);
}

// Función para remover/borrar un paquete del sistema
bool remove_package(const std::string& pkg_name) {
    std::cout << "[*] Eliminando el paquete '" << pkg_name << "' de Tango Linux..." << std::endl;
    
    std::string db_file = "/var/lib/tango/installed/" + pkg_name + ".list";
    if (file_exists(db_file)) {
        std::string cmd = "xargs rm -f < " + db_file + " 2>/dev/null";
        system(cmd.c_str());
        remove(db_file.c_str());
        std::cout << "[✓] Paquete '" << pkg_name << "' eliminado correctamente.\n";
        return true;
    }

    std::cout << "[!] No se encontró base de datos de rastreo para " << pkg_name << ". Intentando limpieza genérica...\n";
    std::string remove_bin = "rm -f /usr/bin/" + pkg_name;
    system(remove_bin.c_str());
    std::cout << "[✓] Intentada la eliminación de /usr/bin/" << pkg_name << "\n";
    return true;
}

// Sincronizar / Actualizar la base de datos de los mirrors
void sync_databases(const std::string& selected_mirror) {
    std::cout << "[*] Sincronizando repositorios desde: " << selected_mirror << std::endl;
    system("mkdir -p /var/lib/tango/sync");
    
    std::string db_name = "core.db";
    if (selected_mirror.find("/extra/") != std::string::npos) {
        db_name = "extra.db";
    }

    std::string db_url = selected_mirror + db_name;
    std::string out_path = "/var/lib/tango/sync/" + db_name;

    if (download_file(db_url, out_path)) {
        std::cout << "[✓] Sincronización exitosa (" << db_name << "). Base de datos actualizada.\n";
    } else {
        std::cerr << "[X] Falló la sincronización con el mirror seleccionado.\n";
    }
}

// Búsqueda y preparación
bool fetch_and_prepare(const std::string& pkg_name, const std::string& out_tango_path, const std::string& mirror) {
    PackageInfo info;
    if (!query_package_info(pkg_name, info)) {
        std::cerr << "[X] No se pudo encontrar el paquete '" << pkg_name << "' en la red." << std::endl;
        return false;
    }

    std::vector<PackageInfo> pkgs = {info};
    if (!confirm_prompt(pkgs)) {
        std::cout << "[!] Operación cancelada por el usuario." << std::endl;
        return false;
    }

    std::string arch_pkg_url = mirror + info.filename;
    std::cout << "[+] Descargando paquete: " << info.filename << std::endl;
    
    std::string temp_download = "/tmp/" + info.filename;
    if (download_file(arch_pkg_url, temp_download)) {
        return convert_arch_to_tango(temp_download, out_tango_path);
    }

    return false;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Uso: " << argv[0] << " <opción|paquete>\n";
        std::cout << "Opciones:\n";
        std::cout << "  install <paquete>  Instala un paquete con prompt interactivo\n";
        std::cout << "  remove  <paquete>  Elimina un paquete del sistema\n";
        std::cout << "  sync               Sincroniza bases de datos de mirrors\n";
        std::cout << "  update             Actualiza la lista de paquetes eligiendo mirror\n";
        return 1;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);
    MirrorConfig cfg = load_config("/etc/roger/tmirrors.tango");
    std::string action = argv[1];

    if (action == "sync" || action == "update") {
        std::string selected_mirror = select_mirror_dialog(cfg.arch_mirrors);
        sync_databases(selected_mirror);
        curl_global_cleanup();
        return 0;
    }

    if (action == "remove" || action == "delete") {
        if (argc < 3) {
            std::cerr << "[X] Debe especificar el paquete a borrar. Ejemplo: " << argv[0] << " remove htop\n";
            curl_global_cleanup();
            return 1;
        }
        remove_package(argv[2]);
        curl_global_cleanup();
        return 0;
    }

    std::string input = (action == "install" && argc >= 3) ? argv[2] : argv[1];

    if (input.find(".tango.tar.zst") != std::string::npos && file_exists(input)) {
        if (install_tango_pkg(input)) {
            std::cout << "[✓] Paquete local instalado exitosamente." << std::endl;
        } else {
            std::cerr << "[X] Error instalando archivo local." << std::endl;
        }
        curl_global_cleanup();
        return 0;
    }

    std::string selected_mirror = cfg.arch_mirrors[0];
    std::string tango_pkg_file = "/tmp/" + input + ".tango.tar.zst";

    if (fetch_and_prepare(input, tango_pkg_file, selected_mirror)) {
        if (install_tango_pkg(tango_pkg_file)) {
            std::cout << "[✓] ¡" << input << " fue instalado con éxito!" << std::endl;
        } else {
            std::cerr << "[X] Falló la instalación en el sistema." << std::endl;
        }
    }

    curl_global_cleanup();
    return 0;
}
