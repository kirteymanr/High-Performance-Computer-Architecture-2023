//*****************************************************************Author: RAGHAV GOYAL**********************************************************
//************************************************************Scaled Neural Branch Predictor*****************************************************
 

#include "ooo_cpu.h"
#include <stdlib.h>
#include <time.h>
#include <cmath>


//HYBRID VARIABLES THAT CAN ONLY BE DEFINED BEFORE THE PREDICTOR'S CODE
uint8_t v4, gshare;


//**********************Note****************************
// Branch address history- fresh values at beginning of array
// branch history- fresh values at end of number



// HERE STARTS V4-------------------------------------------------------------------------------------------------------------------------------------



//****************Global History**************************
#define EXPANDED_BRANCH_HISTORY_LENGTH 128
#define GLOBAL_BRANCH_HISTORY_LENGTH 40
#define HISTORY_BITS_PER_TABLE 8

//****************Branch Address History******************
#define BRANCH_ADDRESS_HISTORY_LENGTH 128 //bit length
#define BITS_PER_BRANCH_BRANCH_ADDRESS_HISTORY 4//32 branches, 4 lower bits for each

//****************Weight parameters*************************
#define W_COLUMNS 129 // (1 bias column + 128 correlating weights columns)
#define CORRERALTING_WEIGHT_COLUMNS 128
#define COLUMNS_PER_TABLE 8
#define ROWS_RECENT_BRANCHES 512 //W[0..7]
#define ROWS_PAST_BRANCHES 256 //W[8..127]
#define ROWS_BIAS 2048
#define BITS_PER_WEIGHT_RECENT_BRANCHES_EXTENDED 7 //W[0..56], bias column + first 7 tables
#define BITS_PER_WEIGHT_PAST_BRANCHES_SHRINKED 6 //W[57..128], left 9 tables
#define NUM_LARGE_WEIGHT_TABLES 7
#define NUM_SMALL_WEIGHT_TABLES 9
#define MAX_RECENT 63
#define MIN_RECENT -64
#define MAX_PAST 31
#define MIN_PAST -32
#define BITS_PER_WEIGHT_BIAS 7
#define WEIGHTS_PER_PERCEPTRON 8
#define NO_OF_TABLES 16
#define TABLES_PAST 15

//**********************Dynamic Weight Scaling Factor************
#define weight_scaling_factor 1.000673

//*****************Threshold Parameter*************************
#define THETA_INTITIAL  ((int)(1.93*GLOBAL_BRANCH_HISTORY_LENGTH+14))
#define THRESHOLD_COUNTER_BITS 8
#define THRESHOLD_COUNTER_POS_SATURATE 127
#define THRESHOLD_COUNTER_NEG_SATURATE -128

//*******************Hash bits**********************************
#define HASH_BITS_RECENT_BRANCHES 9
#define HASH_BITS_PAST_BRANCHES 8

//*********************Buffer size*****************************
#define NUM_UPDATE_ENTRIES	100

//**********************Global CPU variable********************
int current_cpu;


int get_abslowerfrom_bits(int bits){
    int max=(1<<bits)-1;
    return max+1;
}

//****************Perceprtron***********************************
struct perceptron{
    short int weights[WEIGHTS_PER_PERCEPTRON]; //short int-> 16 bits
};

//******************Previous states of perceptrons**************
struct prev_state{
    bool prediction; //bool->8 bits
    int output; //int->32 bits
    short* bias_weight_address; 
    perceptron* predictors[NO_OF_TABLES];
    bool history_state[EXPANDED_BRANCH_HISTORY_LENGTH];
};

//*********************Buffer for storing previous states***************
prev_state prev_state_buffer[NUM_CPUS][NUM_UPDATE_ENTRIES];
int prev_state_buffer_counter[NUM_CPUS];

//*********************************************************MEMORY UNITS****************************************************
perceptron table_recent[NUM_CPUS][ROWS_RECENT_BRANCHES];
perceptron table_past[NUM_CPUS][TABLES_PAST][ROWS_PAST_BRANCHES];
short int bias_weights[NUM_CPUS][ROWS_BIAS];
unsigned long long int speculative_global_history[NUM_CPUS],global_history[NUM_CPUS]; 
bool speculative_branch_address[NUM_CPUS][BRANCH_ADDRESS_HISTORY_LENGTH],branch_address[NUM_CPUS][BRANCH_ADDRESS_HISTORY_LENGTH];
double weight_factors[NUM_CPUS][CORRERALTING_WEIGHT_COLUMNS];
short int threshold_counter[NUM_CPUS];
short int theta[NUM_CPUS];

//**********************************************************MEMORY UNITS end****************************************************

//****************************pointe to last used state***************
prev_state* store_last_pred[NUM_CPUS];

//*******************************************************************INITIALISATION UTILITY************************************

void initialize_perceptron(perceptron* p){
    for (int i=0;i<WEIGHTS_PER_PERCEPTRON;i++){
        p->weights[i]=0;
    }
}

void initialize_weights(){
    for (int i=0;i<ROWS_RECENT_BRANCHES;i++){
        initialize_perceptron(&table_recent[current_cpu][i]);
    }

    for (int i=0;i<TABLES_PAST;i++){
        for (int j=0;j<ROWS_PAST_BRANCHES;j++){
            initialize_perceptron(&table_past[current_cpu][i][j]);
        }
    }

    for (int i=0;i<ROWS_BIAS;i++){
        bias_weights[current_cpu][i]=0;
    }
}

void initialize_history(){
    speculative_global_history[current_cpu]=0;
    global_history[current_cpu]=0;
    for (int i=0;i<BRANCH_ADDRESS_HISTORY_LENGTH;i++){
        speculative_branch_address[current_cpu][i]=0;
        branch_address[current_cpu][i]=0;
    }
}

void initialize_weight_factors(){
    float a=0.1111,b=0.037;
    for (int i=0;i<CORRERALTING_WEIGHT_COLUMNS;i++){
        weight_factors[current_cpu][i]=1/(a+b*i);
    }
}

void initialize_threshold(){
    theta[current_cpu]=THETA_INTITIAL;
}

//*******************************************************************INITIALISATION UTILITY <---------************************************


//*******************************************************************INITIALISE BRANCH PREDICTOR*****************************************
void initialize_branch_predictor_v4(){ // this function now does not belong to O3_CPU class
    int cpu = 0;
    current_cpu=cpu;
    prev_state_buffer_counter[cpu]=0;
    initialize_weights();
    initialize_history();
    initialize_weight_factors();
    initialize_threshold();
}

//*******************************************************************INITIALISE BRANCH PREDICTOR end*****************************************


//*******************************************************************PREDICTION UTILITY************************************************************ 


//------------------------------------------------------------------Hash functions--------------------------------------------
unsigned short int get_mask_lower_bits(uint64_t pc,int bits){
    unsigned short int mask=1;
    for (int i=0;i<bits-1;i++){
        mask=mask<<1;
        mask=mask|1;
    }
    return (pc&mask);
}

unsigned short int get_num_from_branch_address(int start,int bits){
    unsigned short int ans=0;
    for (int i=start;i<start+bits;i++){
        ans=ans<<1;
        ans=(ans|speculative_branch_address[current_cpu][i]);
    }
    return ans;
}

//gives hash(A[i..i + 7])
//Note--> hash not implemented right now
unsigned short int get_mask_address(int table){
    int start=table*COLUMNS_PER_TABLE;
    if (table==0){
        return get_num_from_branch_address(start,HASH_BITS_RECENT_BRANCHES);
    }
    else{
        return get_num_from_branch_address(start,HASH_BITS_PAST_BRANCHES);
    }
    return 0;   
}

//return hash value according to table_no
// k := (hash(A[i..i + 7]) xor pc) mod n
int hash_function(int i,uint64_t pc){
    //for table 1
    if (i==0){
        return (get_mask_address(i)^get_mask_lower_bits(pc,HASH_BITS_RECENT_BRANCHES))%ROWS_RECENT_BRANCHES;
    }
    else{
        return (get_mask_address(i)^get_mask_lower_bits(pc,HASH_BITS_PAST_BRANCHES))%ROWS_PAST_BRANCHES;
    }
    //tables 2-16
    return 0;
}

//------------------------------------------------------------------Hash functions END--------------------------------------------

//-------------------------------------------------History functions----------------------------------------------------------
unsigned short int get_history_segment(int start, int bits){
    unsigned short int history=(speculative_global_history[current_cpu]>>start);
    unsigned short int mask=1;
    for (int i=0;i<bits-1;i++){
        mask=mask<<1;
        mask=mask|1;
    }
    return (history&mask);
    
}

unsigned short int get_history_bits(int table){
    int start;
    if (table%2==0) start=0;
    else start=1+2*table;
    return get_history_segment(start,HISTORY_BITS_PER_TABLE);
}

void store_history(unsigned short int history_bits,int pos){
    for (int i=0;i<HISTORY_BITS_PER_TABLE;i++){
        store_last_pred[current_cpu]->history_state[pos+i]=(history_bits&1);
        history_bits=(history_bits>>1);
    }
}
//-------------------------------------------------History functions end----------------------------------------------------------

//-------------------------------------------------Weight Scaling and Dot Product functions----------------------------------------------------------

// short int weight_scale(short int wt,int table, int pos){
    
//     float a=0.1111,b=0.037;
//     //a=0.9112;
//     //b=0.109;

//     int new_wt=(wt/(a+b*pos));
//     short int ans;
//     if (table<NUM_LARGE_WEIGHT_TABLES){
//         if (!((new_wt>MAX_RECENT)||(new_wt<MIN_RECENT))) ans=new_wt;
//         else{
//             if (new_wt>MAX_RECENT) ans=MAX_RECENT;
//             else if (new_wt<MIN_RECENT) ans=MIN_RECENT;
//         }
//         return ans;
//     }
//     else{
//         if (!((new_wt>MAX_PAST)||(new_wt<MIN_PAST))) ans=new_wt;
//         else{
//             if (new_wt>MAX_PAST) ans=MAX_PAST;
//             else if (new_wt<MIN_PAST) ans=MIN_PAST;
//         }
//         return ans;
//     }
    
//     //return wt;
// }


//-------------------------------------------------Weight Scaling and Dot Product functions end----------------------------------------------------------

//*******************************************************************PREDICTION UTILITY <-----------------************************************************************

//*******************************************************************PREDICT BRANCH*********************************************************
uint8_t predict_branch_v4(uint64_t pc){
    int cpu = 0;
    //Assign pointer to buffer location
    store_last_pred[cpu]=&prev_state_buffer[cpu][prev_state_buffer_counter[cpu]++];
    if (prev_state_buffer_counter[cpu]>=NUM_UPDATE_ENTRIES) prev_state_buffer_counter[cpu]=0;

    int sum=0;
    sum+=bias_weights[cpu][pc%ROWS_BIAS];
    //************store****************
    store_last_pred[cpu]->bias_weight_address=(&(bias_weights[cpu][pc%ROWS_BIAS]));
    //*********************************
    for (int i=0;i<NO_OF_TABLES;i++){
        int k=hash_function(i,pc);
        // This step complete till here--> k := (hash(A[i..i + 7]) xor pc) mod n Select a row in the table
        //***************store*****************************
        if (i==0) store_last_pred[cpu]->predictors[i]=(&table_recent[cpu][k]);
        else store_last_pred[cpu]->predictors[i]=(&table_past[cpu][i-1][k]);
        //************************************************
        unsigned short int history_bits=get_history_bits(i);
        //***************store*****************************
        store_history(history_bits,i*HISTORY_BITS_PER_TABLE);
        //************************************************
        for (int j=0;j<WEIGHTS_PER_PERCEPTRON;j++){
            if (i==0){
                short int scaled_wt=weight_factors[cpu][i*WEIGHTS_PER_PERCEPTRON+j]*table_recent[cpu][k].weights[j];
                
                if (history_bits&1) sum+=scaled_wt;
                else sum-=scaled_wt;

                history_bits=history_bits>>1;
            }
            else{
                short int scaled_wt=weight_factors[cpu][i*WEIGHTS_PER_PERCEPTRON+j]*table_past[cpu][i-1][k].weights[j];
                
                if (history_bits&1) sum+=scaled_wt;
                else sum-=scaled_wt;

                history_bits=history_bits>>1;
            }
        }
    }
    bool prediction=(sum>=0);
    int output=sum;
    store_last_pred[cpu]->prediction=prediction;
    store_last_pred[cpu]->output=output;
    
    uint8_t final_ans=prediction;
    //THE LINE GIVEN BELOW STORES CURRENT PREDICTION OF V4 GLOBALLY
    v4 = prediction;
    return final_ans;
}

//Ref:
// function prediction (pc: integer) : { taken , not taken }
// begin
// sum := W[pc mod n, 0] Initialize to bias weight
// for i in 1 .. h by 8 in parallel For all h/8 weight tables
// k := (hash(A[i..i + 7]) xor pc) mod n Select a row in the table
// for j in 0 .. 7 in parallel For all weights in the row
// q := select history(H, i)[j] Select branch history
// sum := sum +W[k, i + j + 1] × q Add to dot product
// end for
// end for
// if sum >= 0 then Predict based on sum
// prediction := taken
// else
// prediction := not taken
// endif
// end


//**********store states****************
// struct prev_state{
//     bool prediction; //bool->8 bits
//     int output; //int->32 bits
//     short* bias_weight_address; 
//     perceptron* predictors[NO_OF_TABLES];
//     bool history_state[EXPANDED_BRANCH_HISTORY_LENGTH];
// };

//*******************************************************************PREDICT BRANCH <-------------*********************************************************

//*******************************************************************LAST BRANCH UTILITY************************************************************
void clip(short* bias_weight_address,int flag){
    if (flag==0){
        if ((*(bias_weight_address))>MAX_RECENT) (*(bias_weight_address))=MAX_RECENT;
        if ((*(bias_weight_address))<MIN_RECENT) (*(bias_weight_address))=MIN_RECENT;
    }
    else{
        if ((*(bias_weight_address))>MAX_PAST) (*(bias_weight_address))=MAX_PAST;
        if ((*(bias_weight_address))<MIN_PAST) (*(bias_weight_address))=MIN_PAST;
    }
}
//*******************************************************************LAST BRANCH UTILITY <-----------------************************************************************


//*******************************************************************LAST BRANCH RESULT********************************************************
void last_branch_result_v4(uint64_t pc, uint64_t branch_target, uint8_t taken, uint8_t branch_type){

    int cpu = 0;
    bool predicted_outcome=store_last_pred[cpu]->prediction;

    //******************Update branch histories********************
    global_history[cpu]=global_history[cpu]<<1;
    global_history[cpu]=global_history[cpu]|taken;

    if (predicted_outcome!=taken) speculative_global_history[cpu]=global_history[cpu];

    //******************Update branch address histories********************
    for (int i=BRANCH_ADDRESS_HISTORY_LENGTH-BITS_PER_BRANCH_BRANCH_ADDRESS_HISTORY-1;i>=0;i--){
        branch_address[cpu][i+BITS_PER_BRANCH_BRANCH_ADDRESS_HISTORY]=branch_address[cpu][i];
    }
    for (int i=BITS_PER_BRANCH_BRANCH_ADDRESS_HISTORY-1;i>=0;i--){
        branch_address[cpu][i]=(branch_target&1);
        branch_target=branch_target>>1;
    }

    if (predicted_outcome!=taken){
        for (int i=0;i<BRANCH_ADDRESS_HISTORY_LENGTH;i++){
            speculative_branch_address[cpu][i]=branch_address[cpu][i];
        }
    }

    //*******************Check if training needed*******************
    bool istrain=((abs(store_last_pred[cpu]->output)<theta[cpu])||((store_last_pred[cpu]->prediction)!=taken));
    if (!istrain) return;

    //*******************Train bias weight**************************
    if (taken) (*(store_last_pred[cpu]->bias_weight_address))++;
    else (*(store_last_pred[cpu]->bias_weight_address))--;
    clip(store_last_pred[cpu]->bias_weight_address,0); //large weight size

    //*****************Train threshold value************************
    if ((store_last_pred[cpu]->prediction)!=taken){
        threshold_counter[cpu]++;
        if (threshold_counter[cpu]==THRESHOLD_COUNTER_POS_SATURATE){
            theta[cpu]++;
            threshold_counter[cpu]=0;
        }
    }
    else{
        threshold_counter[cpu]--;
        if (threshold_counter[cpu]==THRESHOLD_COUNTER_NEG_SATURATE){
            theta[cpu]--;
            threshold_counter[cpu]=0;
        }
    }

    //*******************Train table weights*************************
    bool* prev_history=store_last_pred[cpu]->history_state;

    for (int i=0;i<NO_OF_TABLES;i++){
        // perceptron* p=store_last_pred[cpu]->predictors[i];
        for (int j=0;j<WEIGHTS_PER_PERCEPTRON;j++){
            if (prev_history[i*HISTORY_BITS_PER_TABLE+j]==taken){
                (*(&(store_last_pred[cpu]->predictors[i]->weights[j])))++;
                weight_factors[cpu][i*WEIGHTS_PER_PERCEPTRON+j]*=weight_scaling_factor;
                //*************clip*********************
                if (i<NUM_LARGE_WEIGHT_TABLES) clip(&(store_last_pred[cpu]->predictors[i]->weights[j]),0); //large weight size
                else clip(&(store_last_pred[cpu]->predictors[i]->weights[j]),1);
            }
            else{
                (*(&(store_last_pred[cpu]->predictors[i]->weights[j])))--;
                weight_factors[cpu][i*WEIGHTS_PER_PERCEPTRON+j]/=weight_scaling_factor;
                //*************clip*********************
                if (i<NUM_LARGE_WEIGHT_TABLES) clip(&(store_last_pred[cpu]->predictors[i]->weights[j]),0); //large weight size
                else clip(&(store_last_pred[cpu]->predictors[i]->weights[j]),1);
            }
        }
    }
}

//**********store states****************
// struct prev_state{
//     bool prediction; //bool->8 bits
//     int output; //int->32 bits
//     short* bias_weight_address; 
//     perceptron* predictors[NO_OF_TABLES];
//     bool history_state[EXPANDED_BRANCH_HISTORY_LENGTH];
// };

//*******************************************************************LAST BRANCH RESULT <----------------******************************************************


// HERE ENDS V4 -----------------------------------------------------------------------------------------------------------------------------------------------------------


// HERE STARTS GSHARE -----------------------------------------------------------------------------------------------------------------------------------------------------
#define GLOBAL_HISTORY_LENGTH 14
#define GLOBAL_HISTORY_MASK (1 << GLOBAL_HISTORY_LENGTH) - 1
int branch_history_vector[NUM_CPUS];

#define GS_HISTORY_TABLE_SIZE 16384
int gs_history_table[NUM_CPUS][GS_HISTORY_TABLE_SIZE];
int my_last_prediction[NUM_CPUS];

void initialize_branch_predictor_gshare()
{
    int cpu = 0;
   // cout << "CPU " << cpu << " GSHARE branch predictor" << endl;

    branch_history_vector[cpu] = 0;
    my_last_prediction[cpu] = 0;

    for(int i=0; i<GS_HISTORY_TABLE_SIZE; i++)
        gs_history_table[cpu][i] = 2; // 2 is slightly taken
}

unsigned int gs_table_hash(uint64_t ip, int bh_vector)
{
    int cpu = 0;
    unsigned int hash = ip^(ip>>GLOBAL_HISTORY_LENGTH)^(ip>>(GLOBAL_HISTORY_LENGTH*2))^bh_vector;
    hash = hash%GS_HISTORY_TABLE_SIZE;

    //printf("%d\n", hash);

    return hash;
}

uint8_t predict_branch_gshare(uint64_t ip)
{
    int cpu = 0;
    int prediction = 1;

    int gs_hash = gs_table_hash(ip, branch_history_vector[cpu]);

    if(gs_history_table[cpu][gs_hash] >= 2)
        prediction = 1;
    else
        prediction = 0;

    my_last_prediction[cpu] = prediction;
    // LINE BELOW STORES PREDICTION OF GSHARE GLOBALLY
    gshare = prediction;
    return prediction;
}

void last_branch_result_gshare(uint64_t ip, uint64_t branch_target, uint8_t taken, uint8_t branch_type)
{   int cpu = 0;
    int gs_hash = gs_table_hash(ip, branch_history_vector[cpu]);

    if(taken == 1) {
        if(gs_history_table[cpu][gs_hash] < 3)
            gs_history_table[cpu][gs_hash]++;
    } else {
        if(gs_history_table[cpu][gs_hash] > 0)
            gs_history_table[cpu][gs_hash]--;
    }

    // update branch history vector
    branch_history_vector[cpu] <<= 1;
    branch_history_vector[cpu] &= GLOBAL_HISTORY_MASK;
    branch_history_vector[cpu] |= taken;
}

// HERE ENDS GSHARE PREDICTOR------------------------------------------------------------------------------------------------------------

// HERE STARTS HYBRID PREDICTOR------------------------------------------------------------------------------------------------------------

//Some Constants:
#define hybrid_history 16390

// HYBRID HISTORY
int HYBRID[hybrid_history];

//Hybrid initialize
void O3_CPU::initialize_branch_predictor(){
    initialize_branch_predictor_gshare();
    initialize_branch_predictor_v4();
}

uint8_t O3_CPU::predict_branch(uint64_t ip){
   // uint8_t prediction;
    v4 = predict_branch_v4(ip);
    gshare = predict_branch_gshare(ip);
    long long int mask=1;
    long long int hash_value;
    for(int i=0;i<13;i++)mask = (mask<<1)|1;
    hash_value = mask&ip;
    if(HYBRID[hash_value]<2)
        return v4;  // V4 is predictor 2
    else
        return gshare; // gshare is predictor 1
}

void O3_CPU::last_branch_result(uint64_t ip, uint64_t branch_target, uint8_t taken, uint8_t branch_type)
{
    long long int mask=1;
    long long int hash_value;
    for(int i=0;i<13;i++)mask = (mask<<1)|1;
    hash_value = mask&ip;
    if(gshare!=taken && v4==taken){
        if(HYBRID[hash_value]>0)HYBRID[hash_value] -= 1;
    }
    if(gshare==taken && v4!=taken){
        if(HYBRID[hash_value]<3)HYBRID[hash_value] += 1;
    }

    last_branch_result_gshare(ip,branch_target, taken,branch_type);
    last_branch_result_v4(ip,branch_target, taken,branch_type);
}

// HERE ENDS HYBRID PREDICTOR------------------------------------------------------------------------------------------------------------
