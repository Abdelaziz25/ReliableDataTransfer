#include <utility>
#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <netdb.h>
#include <string>
#include <thread>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <chrono>
#include <bits/stdc++.h>

using namespace std;

static const int MSS = 508;
static const int ACK_PACKET_SIZE = 8;
static const int CHUNK_SIZE = 499;

struct packet {
    uint16_t cksum;
    uint16_t len;
    uint32_t seqno;
    char data [500];
};

struct not_sent_packet{
    int seqno;
    chrono::time_point<chrono::system_clock> timer;
    bool done;
};

struct ack_packet {
    uint16_t cksum;
    uint16_t len;
    uint32_t ackno;
};

void handle_client_request(int server_socket, int client_fd, sockaddr_in client_addr, char rec_buffer [] , int bufferSize);
long checkFileExistence(string fileName);
vector<string> readDataFromFile(string fileName);
void sendTheData_HandleCongesion (int client_fd, struct sockaddr_in client_addr , vector<string> data);
packet create_packet_data(string packetString, int seqNum);
bool send_packet(int client_fd, struct sockaddr_in client_addr , string temp_packet_string, int seqNum);
vector<string> readArgsFile();
bool DropTheDatagram();
bool CorruptTheDatagram();
uint16_t get_data_checksum (string content, uint16_t len , uint32_t seqno);
uint16_t get_ack_checksum (uint16_t len , uint32_t ackno);

enum fsm_state {slow_start, congestion_avoidance, fast_recovery};
int port, RandomSeedGen;
double PLP;
vector<not_sent_packet> not_sent_packets;
vector<packet> sent_packets;

vector<string> readCommand(){
    string fileName = "command.txt";
    vector<string> commands;
    string line;
    ifstream f;
    f.open(fileName);
    while(getline(f, line))
    {
        commands.push_back(line);
    }
    return commands;
}

int main()
{

    vector<string> the_args = readCommand();
    port = stoi(the_args[0]);
    RandomSeedGen = stoi(the_args[1]);
    PLP = stod(the_args[2]);
    //PLP = stod("0");
    srand(RandomSeedGen);

    int server_socket, client_socket;
    int portNom = 8000;

    struct sockaddr_in server_address{};
    struct sockaddr_in client_address{};
    int server_addrlen = sizeof(server_address);

    if ((server_socket = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0)
    {
        perror("error creating server socket ! ");
        exit(1);
    }

    memset(&server_address, 0, sizeof(server_address));
    memset(&client_address, 0, sizeof(client_address));
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(portNom);
    server_address.sin_addr.s_addr = INADDR_ANY;
    memset(&(server_address.sin_zero), '\0', ACK_PACKET_SIZE);

    if (bind(server_socket, (struct sockaddr *) &server_address, sizeof(server_address)) < 0)
    {
        perror("error in binding server ! ");
        exit(1);
    }

    while (true){
        socklen_t client_addrlen = sizeof(struct sockaddr);
        cout << "Waiting For A New Connection ... " << endl << flush;
        char rec_buffer[MSS];
        ssize_t Received_bytes = recvfrom(server_socket, rec_buffer, MSS, 0, (struct sockaddr*)&client_address, &client_addrlen);
        if (Received_bytes <= 0){
             perror("error in receiving bytes of the file name !");
             exit(1);
        }
        /** forking to handle request **/
         pid_t pid = fork();
         if (pid == -1){
             perror("error in forking a child process for the client ! ");
         } else if (pid == 0){

            if ((client_socket = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0)
            {
                perror("error creating a socket for the client ! ");
                exit(1);
            }
             handle_client_request(server_socket,client_socket, client_address, rec_buffer , MSS);
             exit(0);
         }

    }
    close(server_socket);
    return 0;
}

void handle_client_request(int server_socket, int client_fd, struct sockaddr_in client_addr, char rec_buffer [] , int bufferSize) {

     auto* data_packet = (struct packet*) rec_buffer;
     string fileName = string(data_packet->data);
     cout << "requested file name from client  : " << fileName <<"\n" << " , Lenght : " << fileName.size() << endl;
     int fileSize = checkFileExistence(fileName);
     if (fileSize == -1){
        return;
     }
     int numberOfPackets = ceil(fileSize * 1.0 / CHUNK_SIZE);
     cout << "File Size : " << fileSize << " Bytes , Num. of chuncks : " << numberOfPackets << endl << flush;

     /** send ack to file name **/
     struct ack_packet ack;
     ack.cksum = 0;
     ack.len = numberOfPackets;
     ack.ackno = 0;
     char* buf = new char[MSS];
     memset(buf, 0, MSS);
     memcpy(buf, &ack, sizeof(ack));
     ssize_t bytesSent = sendto(client_fd, buf, MSS, 0, (struct sockaddr *)&client_addr, sizeof(struct sockaddr));
     if (bytesSent == -1) {
        perror("Error Sending The Ack ! ");
        exit(1);
     } else {
        cout << "Ack of file name is sent successfully" << endl << flush;
     }

     /** read data from file **/
     vector<string> DataPackets = readDataFromFile(fileName);
     if (DataPackets.size() == numberOfPackets){
        cout << "File Data is read successfully " << endl << flush;
     }

     /** start sending data and handling congestion control using the SM **/
     sendTheData_HandleCongesion(client_fd, client_addr, DataPackets);

}

void sendTheData_HandleCongesion (int client_fd, struct sockaddr_in client_addr , vector<string> data){
    ofstream myFile_Handler;
    // File Open
    myFile_Handler.open("File_1.txt");

    int cwnd_base = 0;
    double cwnd = 1;
    //cout << "CWND : " << cwnd << " MSS "<< endl << flush;
    // Write to the file
    myFile_Handler << cwnd << endl;
    int base_packet_number = 0;
    long sentBytes = 0;
    int sst = 128;
    bool flag = true;
    int seqNum = 0;
    long sentPacketsNotAcked = 0;
    fsm_state st = slow_start;
    long numberOfDupAcks = 0;
    int lastAckedSeqNum = -1;
    bool stillExistAcks = true;
    char rec_buf[MSS];
    socklen_t client_addrlen = sizeof(struct sockaddr);
    int totalPackets = data.size();
    int alreadySentPackets = 0;

    while (flag){

        /**
        this part will run first to send first datagram as stated in pdf.
        **/
        while(cwnd_base < cwnd && alreadySentPackets + not_sent_packets.size() < totalPackets){
            seqNum = base_packet_number + cwnd_base;
            string temp_packet_string = data[seqNum];
            /**
                in case error simulated won't send the packet so the seqnumber will not correct at the receiver so will send duplicate ack.
            **/
            bool isSent = send_packet(client_fd, client_addr, temp_packet_string,seqNum);
            if (isSent == false) {
                perror("Error sending data packet ! ");
                //exit(1);
            } else {
                sentPacketsNotAcked++;
                alreadySentPackets++;
                cout << "Sent Seq Num : " << seqNum << endl << flush;
            }
            cwnd_base++;
        }
	    //cout << "///////////////////////////////////"<< "CWND : " << cwnd << " MSS "<< endl << flush;
	    //cout << "///////////////////////////////////"<< " base : " << cwnd_base << endl << flush;

        /*** receiving ACKs ***/
        if (sentPacketsNotAcked > 0){
            stillExistAcks = true;
            while (stillExistAcks){
                cout << "waiting acwnd_baseck " << endl << flush;
                ssize_t Received_bytes = recvfrom(client_fd, rec_buf, ACK_PACKET_SIZE, 0, (struct sockaddr*)&client_addr, &client_addrlen);
                if (Received_bytes < 0){
                     perror("error receiving bytes ! ");
                     exit(1);
                }
                else if (Received_bytes != ACK_PACKET_SIZE){
                     cout << "Expecting Ack Got Something Else" << endl << flush;
                     exit(1);
                }
                else {

                    auto ack = (ack_packet*) malloc(sizeof(ack_packet));
                    memcpy(ack, rec_buf, ACK_PACKET_SIZE);
                    cout << "Ack. " << ack->ackno << " Received." << endl << flush;

                    if (get_ack_checksum(ack->len, ack->ackno) != ack->cksum){
                        cout << "Corrupt Ack. received" << endl << flush ;
                    }

                    int ack_seqno = ack->ackno;
                    if (lastAckedSeqNum == ack_seqno){

                        numberOfDupAcks++;
                        sentPacketsNotAcked--;
                        if (st == fast_recovery){
                            cwnd++;
                            //cout << "CWND : " << cwnd << " MSS "<< endl << flush;
                            // Write to the file
                            myFile_Handler << cwnd << endl;
                        } else if (numberOfDupAcks == 3){
                            sst = cwnd / 2;
                            cwnd = sst + 3;
                            cout << "///////////////////////////////////////////////////////////////// Triple duplicate Ack /////////////////////////////////////////////////////////////////////////" << endl;
                            //cout << "CWND : " << cwnd << " MSS "<< endl << flush;
                            // Write to the file
                            myFile_Handler << cwnd << endl;
                            st = fast_recovery;
                            /** retransmit the lost packet **/
                            seqNum = ack_seqno;
                            bool found = false;
                            for (int j = 0; j < not_sent_packets.size() ;j++){
                                not_sent_packet nspkt = not_sent_packets[j];
                                if (nspkt.seqno == seqNum){
                                      found = true;
                                      string temp_packet_string = data[seqNum];
                                      struct packet data_packet = create_packet_data(temp_packet_string, seqNum);
                                      char sendBuffer [MSS];
                                      memset(sendBuffer, 0, MSS);
                                      memcpy(sendBuffer, &data_packet, sizeof(data_packet));
                                      ssize_t bytesSent = sendto(client_fd, sendBuffer, MSS, 0, (struct sockaddr *)&client_addr, sizeof(struct sockaddr));
                                      if (bytesSent == -1) {
                                        //bool isSent = send_packet(client_fd, client_addr, temp_packet_string,seqNum);
                                        //if (isSent == false) {
                                          perror("error re-sending data packet ! ");
                                          exit(1);
                                      } else {
                                          sentPacketsNotAcked++;
                                          alreadySentPackets++;
                                          not_sent_packets.erase(not_sent_packets.begin() + j);
                                      }
                                      break;
                                }
                            }

                            /** handle checksum error **/
                            if (!found){
                                for (int j = 0; j < sent_packets.size() ;j++){
                                    packet spkt = sent_packets[j];
                                    if (spkt.seqno == seqNum){
                                          found = true;
                                          string temp_packet_string = data[seqNum];
                                          struct packet data_packet = create_packet_data(temp_packet_string, seqNum);
                                          char sendBuffer [MSS];
                                          memset(sendBuffer, 0, MSS);
                                          memcpy(sendBuffer, &data_packet, sizeof(data_packet));
                                          ssize_t bytesSent = sendto(client_fd, sendBuffer, MSS, 0, (struct sockaddr *)&client_addr, sizeof(struct sockaddr));
                                          if (bytesSent == -1) {
                                          //bool isSent = send_packet(client_fd, client_addr, temp_packet_string,seqNum);
                                          //if (isSent == false) {
                                              perror("error re-sending data packet ! ");
                                              exit(1);
                                          } else {
                                              alreadySentPackets++;
                                              sent_packets.erase(sent_packets.begin() + j);
                                          }
                                          break;
                                    }
                                }

                            }

                        }
                        //cout << "CWND : " << cwnd << " MSS "<< endl << flush;

                    } else if (lastAckedSeqNum < ack_seqno) {
                        /** new ack : compute new base and packet no. and handling congestion control FSM **/
                        cout << "newAck " << endl;
                        numberOfDupAcks = 0;
                        lastAckedSeqNum = ack_seqno;
                        int advance = lastAckedSeqNum - base_packet_number;
                        cwnd_base = cwnd_base - advance;
                        base_packet_number = lastAckedSeqNum;
                        if (st == slow_start){
                           if (cwnd*2 >= sst){
                                st = congestion_avoidance;
                                cwnd++;
                           }
                           else{
                               cwnd=cwnd*2;
                           }
                           //cout << "CWND : " << cwnd << " MSS "<< endl << flush;
                           // Write to the file
                           myFile_Handler << cwnd << endl;
                           if (cwnd >= sst){
                                st = congestion_avoidance;
                           }
                        } else if (st == congestion_avoidance){
                            cwnd ++;
                            ///cwnd += (1 / floor(cwnd));
                            //cout << "CWND : " << cwnd << " MSS "<< endl << flush;
                            // Write to the file
                            myFile_Handler << cwnd << endl;
                        } else if (st == fast_recovery){
                            st = congestion_avoidance;
                            cwnd = sst;
                            //cout << "CWND : " << cwnd << " MSS "<< endl << flush;
                            // Write to the file
                            myFile_Handler << cwnd << endl;
                        }
                        //cout << "CWND : " << cwnd << " MSS "<< endl << flush;
                        //cout << " base : " << cwnd_base << endl << flush;
                        //cout << " packet : " << base_packet_number << endl << flush;
                        sentPacketsNotAcked--;
                    } else {
                        sentPacketsNotAcked--;
                        //cout << "CWND : " << cwnd << " MSS "<< endl << flush;
                    }

                    if (sentPacketsNotAcked == 0){
                        //cout << "CWND : " << cwnd << " MSS "<< endl << flush;
                        stillExistAcks = false;
                    }

                }

            }

        }


        /** Handle Time Out **/
        bool entered=false;
        for (int j = 0; j < not_sent_packets.size() ;j++){
            not_sent_packet nspkt = not_sent_packets[j];
            chrono::time_point<chrono::system_clock> current_time = chrono::system_clock::now();
            chrono::duration<double> elapsed_time = current_time - nspkt.timer;
            if (elapsed_time.count() >= 2){
                 entered=true;

                //  cwnd=1;
                //  //cwnd_base=0;
                //  sst=128;
                //  st = slow_start;
                //  //cout << "CWND : " << cwnd << " MSS "<< endl << flush;
                //  // Write to the file
                //  myFile_Handler << cwnd << endl;
                
                 cout << "Timed Out ! " << endl << flush;
                 cout << "Re-transmitting the packet " << endl << flush;
                 seqNum = nspkt.seqno;
                 string temp_packet_string = data[seqNum];
                 struct packet data_packet = create_packet_data(temp_packet_string, seqNum);
                 char sendBuffer [MSS];
                 memset(sendBuffer, 0, MSS);
                 memcpy(sendBuffer, &data_packet, sizeof(data_packet));
                 ssize_t bytesSent = sendto(client_fd, sendBuffer, MSS, 0, (struct sockaddr *)&client_addr, sizeof(struct sockaddr));
                 if (bytesSent == -1) {
                        perror("error resending the data packet ! ");
                        exit(1);
                 } else {
                        sentPacketsNotAcked++;
                        alreadySentPackets++;
                        not_sent_packets.erase(not_sent_packets.begin() + j);
                        j--;
                        cout << "Sent Seq Num : " << seqNum << endl << flush;
                 }
            }
        }
        if(entered){
            entered=false;
            cwnd=1;
            //cwnd_base=0;
            st = slow_start;
            //cout << "CWND : " << cwnd << " MSS "<< endl << flush;
            // Write to the file
            myFile_Handler << cwnd << endl;
        }

    }

    // File Close
    myFile_Handler.close();
}

bool send_packet(int client_fd, struct sockaddr_in client_addr , string temp_packet_string, int seqNum){
     char sendBuffer [MSS];
     struct packet data_packet = create_packet_data(temp_packet_string, seqNum);
     bool corrupt=CorruptTheDatagram();
     if(corrupt){
         data_packet.cksum=data_packet.cksum-1;
     }
     memset(sendBuffer, 0, MSS);
     memcpy(sendBuffer, &data_packet, sizeof(data_packet));
     //cout << data_packet.data << endl;
     if (!DropTheDatagram()&&!corrupt){
         ssize_t bytesSent = sendto(client_fd, sendBuffer, MSS, 0, (struct sockaddr *)&client_addr, sizeof(struct sockaddr));
         if (bytesSent == -1) {
                return false;
         } else {
                sent_packets.push_back(data_packet);
                return true;
         }
     } else {
        cout << "///////////////////////////////////Drop data" << endl; 
        struct not_sent_packet nspacket{};
        nspacket.seqno = seqNum;
        nspacket.done = false;
        nspacket.timer = chrono::system_clock::now();
        not_sent_packets.push_back(nspacket);

        return false;
     }
}

packet create_packet_data(string packetString, int seqNum) {

    struct packet p{};
    memset(p.data,0,500);
    strcpy(p.data, packetString.c_str());
    //p.data = &packetString[0];
    p.seqno = seqNum;
    p.len = packetString.size();
    //cout << "plen : " << p.len << endl;
    p.cksum = get_data_checksum(packetString,p.len,p.seqno);
    return p;

}


long checkFileExistence(string fileName){
     ifstream file(fileName.c_str(), ifstream::ate | ifstream::binary);
     if (!file.is_open()) {
        cout << "Error Open the requested file" << endl << flush;
        return -1;
     }
     cout << "the File is opened successfully" << endl << flush;
     long len = file.tellg();
     file.close();
     return len;
}

vector<string> readDataFromFile(string fileName){

    vector<string> DataPackets;
    string temp = "";
    ifstream fin;
    fin.open(fileName);
    if (fin){
        char c;
        int char_counter = 0;
        while(fin.get(c)){
            if(char_counter < CHUNK_SIZE) {
                temp += c;
            }else{
                DataPackets.push_back(temp);
                temp.clear();
                temp += c;
                char_counter = 0;
                continue;
            }
            char_counter++;
        }
        if (char_counter > 0){
            DataPackets.push_back(temp);
        }
    }
    fin.close();
    return DataPackets;

}

vector<string> readArgsFile(){
    string fileName = "info.txt";
    vector<string> commands;
    string line;
    string content = "";
    ifstream myfile;
    myfile.open(fileName);
    while(getline(myfile, line))
    {
        commands.push_back(line);
    }
    return commands;
}

uint16_t get_data_checksum (string content, uint16_t len , uint32_t seqno){

    uint32_t sum = 0;
    sum += len;
    sum += seqno;
    int n = content.length();
    char arr[n+1];
    strcpy(arr, content.c_str());
    //cout << " n : " << n << endl;
    //cout << "last : " << arr[n-1] << endl;
    //cout << "exact : " << content[n-1] << endl;
    for (int i = 0; i < n; i++){
        sum += arr[i];
    }
    while (sum >> 16){
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    uint16_t OCSum = (uint16_t) (~sum);
    return OCSum;
}

bool DropTheDatagram(){
    int res = rand() % 100;
    double isLost = res * PLP;
    cout << "is Lost val : " << isLost << endl << flush;
    if (isLost >= 5.9){
        return true;
    }
    return false;
}

bool CorruptTheDatagram(){
    int res = rand() % 100;
    double isLost = res * PLP;
    cout << "is Lost val : " << isLost << endl << flush;
    if (isLost >= 5.9){
        return true;
    }
    return false;
}

uint16_t get_ack_checksum (uint16_t len , uint32_t ackno){

    uint32_t sum = 0;
    sum += len;
    sum += ackno;
    while (sum >> 16){
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    uint16_t OCSum = (uint16_t) (~sum);
    return OCSum;
}
