#include <maxtest/nodes.hh>

#include <algorithm>
#include <sstream>
#include <cstring>
#include <future>
#include <functional>
#include <iostream>
#include <string>
#include <csignal>
#include <maxtest/envv.hh>
#include <maxbase/format.hh>

using std::string;

Nodes::Nodes(const char* prefix, const std::string& network_config, bool verbose)
    : verbose(verbose)
    , m_prefix(prefix)
    , network_config(network_config)
{
}

Nodes::~Nodes()
{
    for (auto a : m_ssh_connections)
    {
        pclose(a);
    }
}

bool Nodes::check_node_ssh(int node)
{
    bool res = true;

    if (ssh_node(node, "ls > /dev/null", false) != 0)
    {
        std::cout << "Node " << node << " is not available" << std::endl;
        res = false;
    }

    return res;
}

bool Nodes::check_nodes()
{
    std::vector<std::future<bool>> f;

    for (int i = 0; i < N; i++)
    {
        f.push_back(std::async(std::launch::async, &Nodes::check_node_ssh, this, i));
    }

    return std::all_of(f.begin(), f.end(), std::mem_fn(&std::future<bool>::get));
}

string Nodes::generate_ssh_cmd(int node, const string& cmd, bool sudo)
{
    string rval;
    if (m_ip4[node] == "127.0.0.1")
    {
        // If node is the local machine, run command as is.
        rval = sudo ? (m_access_sudo[node] + " " + cmd) : cmd;
    }
    else
    {
        // Run command through ssh. The ControlMaster-option enables use of existing pooled connections,
        // greatly speeding up the operation.
        string p1 = mxb::string_printf("ssh -i %s ", m_sshkey[node].c_str());
        string p2 = "-o UserKnownHostsFile=/dev/null "
                    "-o CheckHostIP=no "
                    "-o ControlMaster=auto "
                    "-o ControlPath=./maxscale-test-%r@%h:%p "
                    "-o ControlPersist=yes "
                    "-o StrictHostKeyChecking=no "
                    "-o LogLevel=quiet ";

        string p3 = mxb::string_printf("%s@%s ", m_access_user[node].c_str(), m_ip4[node].c_str());
        string p4 = sudo ? mxb::string_printf("'%s %s'", m_access_sudo[node].c_str(), cmd.c_str()) :
            mxb::string_printf("'%s'", cmd.c_str());
        rval = p1 + p2 + p3 + p4;
    }
    return rval;
}

FILE* Nodes::open_ssh_connection(int node)
{
    std::ostringstream ss;

    if (m_ip4[node] == "127.0.0.1")
    {
        ss << "bash";
    }
    else
    {
        ss << "ssh -i " << m_sshkey[node] << " "
           << "-o UserKnownHostsFile=/dev/null "
           << "-o StrictHostKeyChecking=no "
           << "-o LogLevel=quiet "
           << "-o CheckHostIP=no "
           << "-o ControlMaster=auto "
           << "-o ControlPath=./maxscale-test-%r@%h:%p "
           << "-o ControlPersist=yes "
           << m_access_user[node] << "@"
           << m_ip4[node]
           << (verbose ? "" :  " > /dev/null");
    }

    return popen(ss.str().c_str(), "w");
}

int Nodes::ssh_node(int node, const char* ssh, bool sudo)
{
    if (verbose)
    {
        std::cout << ssh << std::endl;
    }

    int rc = 1;
    FILE* in = open_ssh_connection(node);

    if (in)
    {
        if (sudo)
        {
            fprintf(in, "sudo su -\n");
            fprintf(in, "cd /home/%s\n", m_access_user[node].c_str());
        }

        fprintf(in, "%s\n", ssh);
        rc = pclose(in);
    }

    if (WIFEXITED(rc))
    {
        return WEXITSTATUS(rc);
    }
    else if (WIFSIGNALED(rc) && WTERMSIG(rc) == SIGHUP)
    {
        // SIGHUP appears to happen for SSH connections
        return 0;
    }
    else
    {
        std::cout << strerror(errno) << std::endl;
        return 256;
    }
}

void Nodes::init_ssh_masters()
{
    std::vector<std::thread> threads;
    m_ssh_connections.resize(N);

    for (int i = 0; i < N; i++)
    {
        threads.emplace_back(
            [this, i]() {
                m_ssh_connections[i] = open_ssh_connection(i);
            });
    }

    for (auto& a : threads)
    {
        a.join();
    }
}

int Nodes::ssh_node_f(int node, bool sudo, const char* format, ...)
{
    va_list valist;
    va_start(valist, format);
    string sys = mxb::string_vprintf(format, valist);
    va_end(valist);
    return ssh_node(node, sys.c_str(), sudo);
}

int Nodes::copy_to_node(int i, const char* src, const char* dest)
{
    if (i >= N)
    {
        return 1;
    }
    char sys[strlen(src) + strlen(dest) + 1024];

    if (m_ip4[i] == "127.0.0.1")
    {
        sprintf(sys,
                "cp %s %s",
                src,
                dest);
    }
    else
    {
        sprintf(sys,
                "scp -q -r -i %s "
                "-o UserKnownHostsFile=/dev/null "
                "-o CheckHostIP=no "
                "-o ControlMaster=auto "
                "-o ControlPath=./maxscale-test-%%r@%%h:%%p "
                "-o ControlPersist=yes "
                "-o StrictHostKeyChecking=no "
                "-o LogLevel=quiet "
                "%s %s@%s:%s",
                m_sshkey[i].c_str(),
                src,
                m_access_user[i].c_str(),
                m_ip4[i].c_str(),
                dest);
    }
    if (verbose)
    {
        printf("%s\n", sys);
    }

    return system(sys);
}


int Nodes::copy_to_node_legacy(const char* src, const char* dest, int i)
{

    return copy_to_node(i, src, dest);
}

int Nodes::copy_from_node(int i, const char* src, const char* dest)
{
    if (i >= N)
    {
        return 1;
    }
    char sys[strlen(src) + strlen(dest) + 1024];
    if (m_ip4[i] == "127.0.0.1")
    {
        sprintf(sys,
                "cp %s %s",
                src,
                dest);
    }
    else
    {
        sprintf(sys,
                "scp -q -r -i %s -o UserKnownHostsFile=/dev/null "
                "-o StrictHostKeyChecking=no "
                "-o LogLevel=quiet "
                "-o CheckHostIP=no "
                "-o ControlMaster=auto "
                "-o ControlPath=./maxscale-test-%%r@%%h:%%p "
                "-o ControlPersist=yes "
                "%s@%s:%s %s",
                m_sshkey[i].c_str(),
                m_access_user[i].c_str(),
                m_ip4[i].c_str(),
                src,
                dest);
    }
    if (verbose)
    {
        printf("%s\n", sys);
    }

    return system(sys);
}

int Nodes::copy_from_node_legacy(const char* src, const char* dest, int i)
{
    return copy_from_node(i, src, dest);
}

int Nodes::read_basic_env()
{
    char env_name[64];
    N = get_N();

    auto prefixc = m_prefix.c_str();

    if ((N > 0) && (N < 255))
    {
        for (int i = 0; i < N; i++)
        {
            // reading IPs
            sprintf(env_name, "%s_%03d_network", prefixc, i);
            m_ip4[i] = get_nc_item(env_name);

            // reading private IPs
            sprintf(env_name, "%s_%03d_private_ip", prefixc, i);
            auto& priv_ip = m_ip_private[i];
            priv_ip = get_nc_item(env_name);
            if (priv_ip.empty())
            {
                priv_ip = m_ip4[i];
            }
            setenv(env_name, priv_ip.c_str(), 1);

            // reading IPv6
            sprintf(env_name, "%s_%03d_network6", prefixc, i);
            auto& ip6 = m_ip6[i];
            ip6 = get_nc_item(env_name);
            if (ip6.empty())
            {
                ip6 = m_ip4[i];
            }
            setenv(env_name, ip6.c_str(), 1);

            //reading sshkey
            sprintf(env_name, "%s_%03d_keyfile", prefixc, i);
            m_sshkey[i] = get_nc_item(env_name);


            sprintf(env_name, "%s_%03d_whoami", prefixc, i);
            auto& access_user = m_access_user[i];
            access_user = get_nc_item(env_name);
            if (access_user.empty())
            {
                access_user = "vagrant";
            }
            setenv(env_name, access_user.c_str(), 1);

            sprintf(env_name, "%s_%03d_access_sudo", prefixc, i);
            m_access_sudo[i] = envvar_get_set(env_name, " sudo ");

            if (access_user == "root")
            {
                m_access_homedir[i] = "/root/";
            }
            else
            {
                m_access_homedir[i] = mxb::string_printf("/home/%s/", access_user.c_str());
            }

            sprintf(env_name, "%s_%03d_hostname", prefixc, i);
            auto& hostname = m_hostname[i];
            hostname = get_nc_item(env_name);
            if (hostname.empty())
            {
                hostname = m_ip_private[i];
            }
            setenv(env_name, hostname.c_str(), 1);

            sprintf(env_name, "%s_%03d_start_vm_command", prefixc, i);
            string start_vm_def = mxb::string_printf("curr_dir=`pwd`; "
                                                     "cd %s/%s;vagrant resume %s_%03d ; "
                                                     "cd $curr_dir",
                                                     getenv("MDBCI_VM_PATH"), getenv("name"), prefixc, i);
            m_start_vm_command[i] = envvar_get_set(env_name, "%s", start_vm_def.c_str());
            setenv(env_name, m_start_vm_command[i].c_str(), 1);

            sprintf(env_name, "%s_%03d_stop_vm_command", prefixc, i);
            string stop_vm_def = mxb::string_printf("curr_dir=`pwd`; "
                                                    "cd %s/%s;vagrant suspend %s_%03d ; "
                                                    "cd $curr_dir",
                                                    getenv("MDBCI_VM_PATH"), getenv("name"), prefixc, i);
            m_stop_vm_command[i] = envvar_get_set(env_name, "%s", stop_vm_def.c_str());
            setenv(env_name, m_stop_vm_command[i].c_str(), 1);
        }
    }

    return 0;
}

const char* Nodes::ip(int i) const
{
    return use_ipv6 ? m_ip6[i].c_str() : m_ip4[i].c_str();
}

std::string Nodes::get_nc_item(const char* item_name)
{
    size_t start = network_config.find(item_name);
    if (start == std::string::npos)
    {
        return "";
    }

    size_t end = network_config.find("\n", start);
    size_t equal = network_config.find("=", start);
    if (end == std::string::npos)
    {
        end = network_config.length();
    }
    if (equal == std::string::npos)
    {
        return "";
    }

    std::string str = network_config.substr(equal + 1, end - equal - 1);
    str.erase(remove(str.begin(), str.end(), ' '), str.end());

    setenv(item_name, str.c_str(), 1);

    return str;
}

int Nodes::get_N()
{
    int n_nodes = 0;
    while (true)
    {
        string item = mxb::string_printf("%s_%03d_network", m_prefix.c_str(), n_nodes);
        if (network_config.find(item) != string::npos)
        {
            n_nodes++;
        }
        else
        {
            break;
        }
    }

    // Is this required?
    string env_name = m_prefix + "_N";
    setenv(env_name.c_str(), std::to_string(n_nodes).c_str(), 1);
    return n_nodes;
}

int Nodes::start_vm(int node)
{
    return (system(m_start_vm_command[node].c_str()));
}

int Nodes::stop_vm(int node)
{
    return (system(m_stop_vm_command[node].c_str()));
}

Nodes::SshResult Nodes::ssh_output(const std::string& cmd, int node, bool sudo)
{
    Nodes::SshResult rval;
    string ssh_cmd = generate_ssh_cmd(node, cmd, sudo);
    FILE* output_pipe = popen(ssh_cmd.c_str(), "r");
    if (!output_pipe)
    {
        printf("Error opening ssh %s\n", strerror(errno));
        return rval;
    }

    const size_t buflen = 1024;
    string collected_output;
    collected_output.reserve(buflen);   // May end up larger.

    char buffer[buflen];
    while (fgets(buffer, buflen, output_pipe))
    {
        collected_output.append(buffer);
    }
    mxb::rtrim(collected_output);
    rval.output = std::move(collected_output);

    int exit_code = pclose(output_pipe);
    rval.rc = (WIFEXITED(exit_code)) ? WEXITSTATUS(exit_code) : 256;
    return rval;
}

bool Nodes::using_ipv6() const
{
    return use_ipv6;
}

const char* Nodes::ip_private(int i) const
{
    return m_ip_private[i].c_str();
}

const char* Nodes::ip6(int i) const
{
    return m_ip6[i].c_str();
}

const char* Nodes::hostname(int i) const
{
    return m_hostname[i].c_str();
}

const char* Nodes::access_user(int i) const
{
    return m_access_user[i].c_str();
}

const char* Nodes::access_homedir(int i) const
{
    return m_access_homedir[i].c_str();
}

const char* Nodes::access_sudo(int i) const
{
    return m_access_sudo[i].c_str();
}

const char* Nodes::sshkey(int i) const
{
    return m_sshkey[i].c_str();
}

const std::string& Nodes::prefix() const
{
    return m_prefix;
}

const char* Nodes::ip4(int i) const
{
    return m_ip4[i].c_str();
}
