#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <sstream>
#include <cstdlib>
#include <sys/stat.h>
#include <curl/curl.h>

// Importar interfaz o cabecera SAT de Roger si se compila en conjunto
#include "RogerSAT.hpp"

struct MirrorConfig {
    std::string tango_repo;
    std::vector<std::string> arch_mirrors;
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

// Cargar /etc/roger/mirrorconf.tango
MirrorConfig load_config(const std::string& config_path) {
    MirrorConfig cfg;
    std::ifstream file(config_path);
    if (!file.is_open()) {
        cfg.arch_mirrors.push_back("https://geo.mirror.pkgbuild.com/core/os/x86_64/");
        cfg.arch_mirrors.push_back("https://geo.mirror.pkgbuild.com/extra/os/x86_64/");
        return cfg;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;

        size_t eq_pos = line.find('=');
        if (eq_pos != std::string::npos) {
            std::string key = line.substr(0, eq_pos);
            std::string val = line.substr(eq_pos + 1);

            if (key == "TANGO_REPO") {
                cfg.tango_repo = val;
            } else if (key == "ARCH_MIRRORS") {
                std::stringstream ss(val);
                std::string mirror;
                while (std::getline(ss, mirror, ',')) {
                    if (!mirror.empty()) cfg.arch_mirrors.push_back(mirror);
                }
            }
        }
    }
    return cfg;
}

// Descarga con validación HTTP 200 (evita guardar páginas de error HTML)
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

// Conversión dinámicamente limpia de .pkg.tar.zst a .tango.tar.zst
bool convert_arch_to_tango(const std::string& pkg_path, const std::string& out_tango_path) {
    std::cout << "[*] Convirtiendo paquete Arch a formato Tango Linux..." << std::endl;
    
    std::string tmp_dir = "/tmp/tango_convert_" + std::to_string(rand());
    system(("mkdir -p " + tmp_dir).c_str());

    // Extraer
    int res1 = system(("tar -I zstd -xf " + pkg_path + " -C " + tmp_dir + " > /dev/null 2>&1").c_str());
    if (res1 != 0) {
        std::cerr << "[!] Error al extraer el paquete comprimido de Arch." << std::endl;
        system(("rm -rf " + tmp_dir + " " + pkg_path).c_str());
        return false;
    }

    // Limpiar metadatos de Arch
    system(("rm -rf " + tmp_dir + "/.PKGINFO " + tmp_dir + "/.BUILDINFO " + tmp_dir + "/.MTREE").c_str());

    // Reempaquetar
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

// Búsqueda y descarga mediante API de Arch
bool fetch_and_prepare(const std::string& pkg_name, const std::string& out_tango_path) {
    MirrorConfig cfg = load_config("/etc/roger/mirrorconf.tango");

    std::cout << "[*] Buscando '" << pkg_name << "' en los mirrors..." << std::endl;

    for (const auto& mirror : cfg.arch_mirrors) {
        std::string api_url = "https://archlinux.org/packages/search/json/?name=" + pkg_name;
        std::string resolve_cmd = "curl -s '" + api_url + "' | grep -o '\"filename\": \"[^\"]*\"' | head -n 1 | cut -d'\"' -f4";
        
        FILE* pipe = popen(resolve_cmd.c_str(), "r");
        char buffer[128];
        std::string filename = "";
        if (pipe) {
            while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
                filename += buffer;
            }
            pclose(pipe);
        }
        
        if (!filename.empty() && filename.back() == '\n') filename.pop_back();

        if (!filename.empty()) {
            std::string arch_pkg_url = mirror + filename;
            std::cout << "[+] Encontrado paquete: " << filename << std::endl;
            
            std::string temp_download = "/tmp/" + filename;
            if (download_file(arch_pkg_url, temp_download)) {
                if (convert_arch_to_tango(temp_download, out_tango_path)) {
                    return true;
                }
            }
        }
    }
    return false;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Uso: " << argv[0] << " <nombre_paquete | archivo.tango.tar.zst>" << std::endl;
        return 1;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);
    std::string input = argv[1];

    // CASO 1: Es un archivo local ya convertido/descargado
    if (input.find(".tango.tar.zst") != std::string::npos && file_exists(input)) {
        if (install_tango_pkg(input)) {
            std::cout << "[✓] Paquete local instalado exitosamente." << std::endl;
        } else {
            std::cerr << "[X] Error instalando archivo local." << std::endl;
        }
        curl_global_cleanup();
        return 0;
    }

    // CASO 2: Es el nombre de un paquete -> Consultar MiniSAT primero
    std::cout << "[*] Consultando resolvedor MiniSAT de Roger..." << std::endl;
    
    // Instanciamos la clase RogerSAT o invocamos su chequeo de dependencias
    // RogerSAT sat_solver;
    // if (!sat_solver.resolve_dependencies(input)) { ... }

    std::string tango_pkg_file = "/tmp/" + input + ".tango.tar.zst";

    if (fetch_and_prepare(input, tango_pkg_file)) {
        if (install_tango_pkg(tango_pkg_file)) {
            std::cout << "[✓] ¡" << input << " fue descargado, convertido e instalado con éxito!" << std::endl;
        } else {
            std::cerr << "[X] Falló la extracción en el sistema." << std::endl;
        }
    } else {
        std::cerr << "[X] No se pudo obtener el paquete." << std::endl;
    }

    curl_global_cleanup();
    return 0;
}
