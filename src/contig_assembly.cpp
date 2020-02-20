#define GRAPH_TRAVERSE

#include "cqf/CQF_mt.h"
#include "base/Utility.h"
#include "base/unordered_map_mt.h"
#include "base/vector_mt.h"
#include "base/Params.h"
#include "base/Hash.h"
#include "base/DNA_string.h"
//#include "base/nthash.h"
#include <boost/iostreams/filter/gzip.hpp>
#include <boost/iostreams/copy.hpp>
#include <boost/iostreams/filtering_streambuf.hpp>
#include <tbb/concurrent_unordered_set.h>
#include <tbb/concurrent_unordered_map.h>
#include <tbb/concurrent_vector.h>
#include <tbb/concurrent_queue.h>
#include <tbb/concurrent_hash_map.h>

//using namespace tbb;
using tbb::concurrent_vector;
using tbb::concurrent_queue;

// struct DNAString_hasher
// {
//     size_t operator()(const DNAString& val)const //A is 00
//     {
//         return MurmurHash2(val.data(), val.length());
//     }
// };

namespace std{
  template<>
  struct hash<DNAString>
  {
     size_t operator () (const DNAString& x) const
     {
        //cout<<"hash being called"<<endl;
        return MurmurHash2(x.data(), x.data_size());
     }
  };

  template<>
  struct equal_to<DNAString>
  {
    bool operator()(const DNAString &lhs, const DNAString &rhs) const 
    {
        //cout<<"equal being called"<<endl;
        return lhs == rhs;
    }
  };
}

namespace tbb{
  template<>
  struct tbb_hash<DNAString>
  { 
    size_t operator () (const DNAString& x) const
     {
        return MurmurHash2(x.data(), x.data_size());
     }
  };
}

struct MyHashCompare {
    static size_t hash( const DNAString& x ) {
      return MurmurHash2(x.data(), x.data_size());
    }
    //! True if strings are equal
    static bool equal( const DNAString& x, const DNAString& y ) {
        return x==y;
    }
};
// struct DNAString_equalto 
// {
//     bool operator()(const DNAString& one, const DNAString& two) const
//     {
//         return (one == two);
//     }
// };

typedef tbb::concurrent_unordered_set<DNAString> unordered_set_mt;
//typedef tbb::concurrent_unordered_map<DNAString, int> unordered_map_mt;
typedef tbb::concurrent_hash_map<DNAString, int, MyHashCompare> hash_map_mt;

//typedef tbb::concurrent_unordered_set<DNAString, DNAString_hasher, DNAString_equalto> unordered_set_mt;
//using namespace boost::program_options;
//namespace po = boost::program_options;
//using boost::program_options::variables_map;

Params get_opts(int argc, char* argv[]){
  namespace po = boost::program_options;
  po::options_description desc(string(argv[0])+"  <options>\nOptions:");
  desc.add_options() 
    ("help,h", "print help messages") 
    (",k", po::value<int>()->required(), "kmer length") 
    ("input,i", po::value<string>()->required(), "a file containing list of input file name(s), should be absolute address or file names when in the running directory.")
    ("format,f", po::value<char>()->default_value('f'), "format of the input: g(gzip); b(bzip2); f(plain fastq)")
    ("cqf,c", po::value<string>()->required(), "the counting quotient filter built with the same 'k'")
    ("abundance_min,s", po::value<int>()->default_value(1), "minimum coverage of k-mers used to extend the assembly") 
    ("solid_abundance_min,x", po::value<int>()->default_value(2), "minimum coverage of a solid k-mer to start the assembly")
    ("solid_abundance_max,X", po::value<int>()->default_value(100), "maximum coverage of a solid k-mer to start the assembly")
    (",t", po::value<int>()->default_value(16), "number of threads")
    ("output,o", po::value<string>()->default_value("unitigs.fa"), "output contig file name (fasta)");
    
  po::variables_map vm;
  po::store(po::parse_command_line(argc, argv, desc), vm);
  
  if (argc==1 || vm.count("help")) {
    cerr << desc << "\n";
    exit(0);
  }
  
  po::notify(vm);

  Params options;
  options.K = vm["-k"].as<int>();
  options.readFileList = vm["input"].as<string>();
  options.cqfFile = vm["cqf"].as<string>();
  options.kmer_abundance_min = vm["abundance_min"].as<int>();
  options.solid_kmer_abundance_min = vm["solid_abundance_min"].as<int>();
  options.solid_kmer_abundance_max = vm["solid_abundance_max"].as<int>();
  options.thread_num = vm["-t"].as<int>();
  options.output = vm["output"].as<string>();
  switch(vm["format"].as<char>()){
    case 'g':
      options.fmode = FILE_MODE::GZIP;
      break;
    case 'b':
      options.fmode = FILE_MODE::BZIP2;
      break;
    case 'f':
      options.fmode = FILE_MODE::TEXT;
      break;
    default:
      std::cerr<<"[Error] unrecognized file format "<<vm["format"].as<char>()<<endl;
      break;
  }

  return options;
}

struct WorkQueue;
struct AssemblyInfo{
  volatile atomic<size_t> seq_num_;
  volatile atomic<size_t> palindrome_seq_num_;
  volatile atomic<size_t> seq_len_in_total_;
  AssemblyInfo(){
    seq_num_ = 0;
    palindrome_seq_num_ = 0;
    seq_len_in_total_ = 0;
  }
};
class WorkQueue2{
public:
  volatile atomic<size_t> start_;//start index of contigs to handle.
  volatile atomic<size_t> step_;//step size.
  volatile atomic<size_t> count_;//next index of non-empty contigs.
  AssemblyInfo assemblyInfo_;
  //volatile atomic<size_t> end;
  boost::mutex mut_;

  //WorkQueue2(): start_(0), step_(10), count_(1){}
  WorkQueue2(size_t start=0, size_t step = 10, size_t count = 1): start_(start), step_(step), count_(count){}
  bool get_work(const concurrent_vector<Contig>& contigs, int& start, int& end, int& counter){
    if(contigs.size() <= start_){
      return false;
    }
    mut_.lock();
    counter = count_;
    start = start_;
    end = min(start_+step_, contigs.size());
    int tmp =0;
    for(int x = start; x < end; x++){
      if(contigs[x].seq.length()!=0){
        tmp++;
      }
    }
    start_ = end;
    count_ += tmp;
    mut_.unlock();
    return true;
  }
};
class WorkQueue3 : public WorkQueue2{
public:
  queue<size_t> count2output;//maintain the output order.
  boost::mutex write_mut_;
  using WorkQueue2::WorkQueue2;
  //WorkQueue3(size_t start=0, size_t step = 10, size_t count = 1): start_(start), step_(step), count_(count){}
  bool get_work(const concurrent_vector<Contig>& contigs, int& start, int& end, int& counter){
    if(contigs.size() <= start_){
      return false;
    }
    mut_.lock();
    while(start_ < contigs.size() && contigs[start_].seq.dna_base_num() == 0){
      start_++;
    }
    if(start_ == contigs.size()){
      return false;
    }
    counter = count_;
    start = start_;
    //end = start_;
    //end = min(start_+step_, contigs.size());
    int tmp =0;
    for(end = start; end < contigs.size() && tmp < step_; end++){
      if(contigs[end].seq.length()!=0){
        tmp++;
      }
    }
    start_ = end;
    count_ += tmp;
    mut_.unlock();

    write_mut_.lock();
    count2output.push(counter);
    write_mut_.unlock();
    return true;
  }
};
struct UnitigNode{
  vector<int> beforeNodes;
  vector<int> afterNodes;
  UnitigNode(){}
};
//void find_unitigs_mt_master(CQF_mt& cqf, const vector<string>& seqFiles, const Params& params, vector_mt<string>& contigs);
//void find_unitigs_mt_master(CQF_mt& cqf, seqFile_batch& seqFiles, const Params& options, vector_mt<Contig>& unitigs);
//void find_unitigs_mt_master(CQF_mt& cqf, seqFile_batch& seqFiles, const Params& options, concurrent_vector<Contig>& unitigs);
void find_unitigs_mt_master(CQF_mt& cqf, seqFile_batch& seqFiles, const Params& options, concurrent_vector<Contig>& unitigs, hash_map_mt& startKmer2unitig);

//void find_unitigs_mt_worker(CQF_mt& cqf, const Params& params, vector_mt<string>& contigs, unordered_map_mt<string, int>& startKmer2unitig, WorkQueue* work_queue);
//void find_unitigs_mt_worker(CQF_mt& cqf, const Params& options, vector_mt<Contig>& contigs, unordered_map_mt<string, int>& startKmer2unitig, WorkQueue* work_queue);

//void find_unitigs_mt_worker(CQF_mt& cqf, seqFile_batch& seqFiles, const Params& options, concurrent_vector<Contig>& contigs, unordered_set_mt& startKmer2unitig, WorkQueue* work_queue);
void find_unitigs_mt_worker(CQF_mt& cqf, seqFile_batch& seqFiles, const Params& options, concurrent_vector<Contig>& contigs, hash_map_mt& startKmer2unitig, WorkQueue* work_queue);

//void get_unitig_forward(CQF_mt& cqf, const Params& params, vector_mt<string>& contigs, unordered_map_mt<string, int>& startKmer2unitig, WorkQueue* work_queue, int contig_id);
//void get_unitig_forward(CQF_mt& cqf, const Params& options, vector_mt<Contig>& contigs, unordered_map_mt<string, int>& startKmer2unitig, WorkQueue* work_queue, const int& contig_id);

//void get_unitig_forward(CQF_mt& cqf, const Params& options, vector_mt<Contig>& contigs, unordered_set_mt& startKmer2unitig, WorkQueue* work_queue, const int& contig_id);

//void get_unitig_forward(CQF_mt& cqf, const Params& options, concurrent_vector<Contig>& contigs, unordered_set_mt& startKmer2unitig, WorkQueue* work_queue, concurrent_vector<Contig>::iterator& contigIter);
void get_unitig_forward(CQF_mt& cqf, const Params& options, concurrent_vector<Contig>& contigs, hash_map_mt& startKmer2unitig, WorkQueue* work_queue, concurrent_vector<Contig>::iterator& contigIter);

//void get_unitig_forward(CQF_mt& cqf, const Params& options, vector_mt<Contig>& contigs, unordered_map_mt<string, int>& startKmer2unitig, WorkQueue* work_queue, Contig& contig);

//void get_unitig_forward(CQF_mt& cqf, const Params& options, vector_mt<Contig>& contigs, unordered_set_mt& startKmer2unitig, WorkQueue* work_queue, Contig& contig);

//void get_unitig_backward(CQF_mt& cqf, const Params& params, vector_mt<string>& contigs, unordered_map_mt<string, int>& startKmer2unitig, WorkQueue* work_queue, int contig_id);
//void get_unitig_backward(CQF_mt& cqf, const Params& options, vector_mt<Contig>& contigs, unordered_map_mt<string, int>& startKmer2unitig, WorkQueue* work_queue, int contig_id);

void track_kmer_worker(const Params& options, const concurrent_vector<Contig>& contigs, hash_map_mt& startKmer2unitig, WorkQueue2* work_queue);
void build_graph_worker(const Params& options, const concurrent_vector<Contig>& contigs, const hash_map_mt& startKmer2unitig, vector<UnitigNode>& unitigNodes, WorkQueue2* work_queue);

int main(int argc, char* argv[]){
  // /*test of DNA string 
  // */
  // string tmp_s("ATGCAGGATGCAT");
  // DNAString dna_seq = tmp_s;
  // dna_seq.append('T');
  // cout<<dna_seq<<endl;
  // cout<<dna_seq.substr(0, 5)<<endl;
  // cout<<dna_seq.substr(7,5)<<endl;
  // if(dna_seq.substr(0,5) == dna_seq.substr(7,5)){
  //   cout<<"Equal operator works"<<endl;
  // }
  // dna_seq.RC();
  // cout<<dna_seq<<endl;
  // //cout<<dna_seq<<endl;
  // //cout<<dna_seq.dna_base_num()<<endl;
  
  // DNAString dna_seq = string("AAATT");
  // cout<<dna_seq.is_palindrome()<<endl;
  // cout<<dna_seq.is_hairpin()<<endl;
  
  // unordered_map<DNAString, int> kmerSet;
  
  // DNAString dna_seq = string("AAATTT");
  
  // char DNA_bases[4]={'A','C','G','T'};
  // DNAString fix = dna_seq.substr(0, 5);
  // DNAString dna_seq1;
  // for(int x = 0; x < 4; x++){
  //   dna_seq1 = dna_seq.substr(0,5).append(DNA_bases[x]);
  //   kmerSet[dna_seq1]=0;
  //   cout<<"Insert "<<dna_seq1<<endl;
  // }

  // for(int x = 0; x < 4; x++){
  //   dna_seq1 = fix;
  //   dna_seq1.append(DNA_bases[x]);
  //   auto it = kmerSet.find(dna_seq1);
  //   if(it != kmerSet.end()){
  //     cout<<dna_seq1<<" found."<<endl;
  //     cout<<it->first<<": "<<it->second<<endl;
  //   }
  // }
  
  // string seqs[4] = {"AAAAAA", "T", "GGGGGGGGGG", "CC"};
  // for (auto seq : seqs){
  //   DNAString dna = string(seq);
  //   cout<<dna<<" is simple "<<dna.is_simple()<<endl;
  // }

  // return 0;

  Params options = get_opts(argc, argv);
  
  vector<string> seqFileNames;
  ifstream fin;
  fin.open(options.readFileList, ios::in);
  string line;
  while(getline(fin, line)){
    if(line.empty())
      continue;
    seqFileNames.push_back(line);
  }
  FILE_TYPE ftype=FILE_TYPE::FASTQ;
  seqFile_batch seqFiles(seqFileNames, ftype, options.fmode);   

  DisplayCurrentDateTime(); 
  cout<<"[CQF] load cqf from disk"<<endl;
  CQF_mt cqf_mt;
  cqf_mt.load(options.cqfFile);
  cout<<"[CQF] cqf loaded!"<<endl;

  // cout<<"[debug] Output information of the cqf"<<endl;
  // cout<<"[debug] Number of elements: "<<cqf_mt.qf->metadata->nelts<<endl;
  // cout<<"[debug] List first 10 elements....\n[debug] ";
  // cqf_mt.reset_iterator();
  // uint64_t key, count;
  // for(int x = 0; x < 10; x++){
  //   cqf_mt.get(key, count);
  //   cout<<key<<":"<<count<<" ";
  //   if(!cqf_mt.next()) break;
  // }
  // cout<<endl;
  // return 0;

  DisplayCurrentDateTime();
  cout<<"[Unitig] find unitigs"<<endl<<std::flush;
  concurrent_vector<Contig> contigs;
  contigs.resize(1);
  hash_map_mt startKmer2unitig;
  find_unitigs_mt_master(cqf_mt, seqFiles, options, contigs, startKmer2unitig);
  
  //bulid the graph
  /*
  if(true){
    cout<<"[Unitig] build compacted DBG graph with unitigs as nodes."<<endl<<std::flush;
    uint64_t totalUnitigs_len, duplicateUnitigs_len; totalUnitigs_len = duplicateUnitigs_len=0;
    int duplicateUnitig_num=0;
    auto contig_num = contigs.size();
    unordered_map<DNAString, int, std::hash<DNAString>> startKmer2unitig;
    vector<bool> isDuplicate(contig_num, false);
    vector<bool> isHairpin(contig_num, false);//whether the non-duplicate unitigs are hairpin(Palindrome excluded).
    vector<int> id2id_afterRemoveDuplicate(contig_num, -1);

    DNAString first_kmer, last_kmer_RC;
    int noduplicate_contig_num=0;
    int palindrome_contig_num, hairpin_contig_num;
    palindrome_contig_num = hairpin_contig_num = 0;
    for(int contig_id = 1; contig_id<contigs.size(); contig_id++){
      if(contigs[contig_id].seq.dna_base_num()==0){
        isDuplicate[contig_id]=true;
        continue;
      }
      totalUnitigs_len += contigs[contig_id].seq.length();
      first_kmer = contigs[contig_id].seq.substr(0,options.K);
      last_kmer_RC = contigs[contig_id].seq.substr(contigs[contig_id].seq.length()-options.K).RC();
      auto it = startKmer2unitig.find(first_kmer);
      if(it != startKmer2unitig.end()){
        // auto tmp = abs(it->second);
        // while(id2id_afterRemoveDuplicate[tmp] != abs(it->second)){
        //     tmp++;
        // }
        
        // cout<<"[Warning] contig "<<(it->second>0?tmp:-tmp)<<" and "<<contig_id<<" share sequences."<<endl;
        // cout<<"[Warning] contig "<<(it->second>0?tmp:-tmp)<<": ";
        // if(it->second < 0){
        //   cout<<RC_DNA(contigs[tmp].seq)<<endl;
        // }else{
        //   cout<<contigs[tmp].seq<<endl;
        // }
        // cout<<"[Warning] contig "<<contig_id<<": "<<contigs[contig_id].seq<<endl;
        duplicateUnitigs_len += contigs[contig_id].seq.length();
        isDuplicate[contig_id]=true;
        duplicateUnitig_num++;
      }else{
        noduplicate_contig_num++;
        id2id_afterRemoveDuplicate[contig_id]=noduplicate_contig_num;
        if(first_kmer == last_kmer_RC){
          if(contigs[contig_id].seq.is_palindrome()){
            //cout<<"[Warning] contig "<<contig_id<<"("<<contigs[contig_id].seq.length()<<" bp) is a palindrome seq (+ and - strand): "<<contigs[contig_id].seq<<endl;
            palindrome_contig_num++;
            startKmer2unitig[first_kmer] = noduplicate_contig_num;
          }else{//Hairpin is the seq where >= K bases at both ends are complementary, but no palindrome.
            //cout<<"[Warning] contig "<<contig_id<<"("<<contigs[contig_id].seq.length()<<" bp) is a hairpin seq (+ and - strand): "<<contigs[contig_id].seq<<endl;
            isHairpin[noduplicate_contig_num] = true;
            hairpin_contig_num++;
            startKmer2unitig[first_kmer] = noduplicate_contig_num;
          }
          //dont't know how to do  
          //cout<<"[Warning] contig "<<contig_id<<" seems like a palindrome seq (+ and - strand): "<<contigs[contig_id].seq<<endl;
        }else{
          startKmer2unitig[first_kmer] = noduplicate_contig_num;
          startKmer2unitig[last_kmer_RC] = -noduplicate_contig_num;
        }
      }

      // if(startKmer2unitig.find(first_kmer) == startKmer2unitig.end()){
      //   cerr<<"[error] why not found!"<<endl;
      // }
      // if(startKmer2unitig.find(last_kmer_RC) == startKmer2unitig.end()){
      //   cerr<<"[error] why not found!"<<endl;
      // }
    }
    cout<<"[Unitig] "<<contigs.size()<<" unitigs reported of length "<<totalUnitigs_len<<" bp in total"<<endl;
    cout<<"[Unitig] Among them, "<<duplicateUnitig_num<<" duplicate unitigs found of length "<<duplicateUnitigs_len<<" bp in total."<<endl;
    cout<<"[Unitig] After removing duplicates, we have "<<noduplicate_contig_num<<" unitigs of length "<<totalUnitigs_len- duplicateUnitigs_len<<" bp in total."<<endl;
    cout<<"[Unitig] Among all non-duplicate unitigs, there are "<<palindrome_contig_num<<" palindromes among "<<hairpin_contig_num+palindrome_contig_num<<" hairpins."<<endl;
  }
  */

  ///*
  //if(false){
    //cout<<"[Unitig] build compacted DBG graph with unitigs as nodes."<<endl;
    uint64_t totalUnitigs_len, duplicateUnitigs_len; totalUnitigs_len = duplicateUnitigs_len=0;
    int duplicateUnitig_num=0;
    auto contig_num = contigs.size();
    //unordered_map<DNAString, int, std::hash<DNAString>> startKmer2unitig;
    //vector<bool> isDuplicate(contig_num, false);
    //vector<bool> isHairpin(contig_num, false);//whether the non-duplicate unitigs are hairpin(Palindrome excluded).
    //vector<int> id2id_afterRemoveDuplicate(contig_num, -1);

    /*
    DNAString first_kmer, last_kmer_RC;
    int noduplicate_contig_num=0;
    int palindrome_contig_num, hairpin_contig_num;
    palindrome_contig_num = hairpin_contig_num = 0;
    for(int contig_id = 1; contig_id<contigs.size(); contig_id++){
      if(contigs[contig_id].seq.dna_base_num()==0){
        continue;
      }
      noduplicate_contig_num++;
      totalUnitigs_len += contigs[contig_id].seq.length();
      first_kmer = contigs[contig_id].seq.substr(0,options.K);
      last_kmer_RC = contigs[contig_id].seq.substr(contigs[contig_id].seq.length()-options.K).RC();
      
      if(first_kmer == last_kmer_RC){
        palindrome_contig_num++;
        //startKmer2unitig[first_kmer] = noduplicate_contig_num;
        hash_map_mt::accessor access;
        if(startKmer2unitig.find(access, first_kmer)){
          access->second = noduplicate_contig_num;
        }else{
          cerr<<"[Error] kmer not found!"<<endl;
        }
        access.release();
        //dont't know how to do  
        //cout<<"[Warning] contig "<<contig_id<<" seems like a palindrome seq (+ and - strand): "<<contigs[contig_id].seq<<endl;
      }else{
        hash_map_mt::accessor access;
        if(startKmer2unitig.find(access, first_kmer)){
          access->second = noduplicate_contig_num;
        }else{
          cerr<<"[Error] kmer not found!"<<endl;
        }
        access.release();
        if(startKmer2unitig.find(access, last_kmer_RC)){
          access->second = -noduplicate_contig_num;
        }else{
          cerr<<"[Error] kmer not found!"<<endl;
        }
        access.release();
        //startKmer2unitig[first_kmer] = noduplicate_contig_num;
        //startKmer2unitig[last_kmer_RC] = -noduplicate_contig_num;
      }
    }
    */
    WorkQueue2* work_queue4trace_kmer = new WorkQueue2();
    boost::thread_group prod_threads;
    for(int t = 0; t<options.thread_num; t++){
      prod_threads.add_thread(new boost::thread(track_kmer_worker, boost::ref(options), boost::ref(contigs), boost::ref(startKmer2unitig), work_queue4trace_kmer));
    }
    prod_threads.join_all();
    cout<<"[Unitig] "<<work_queue4trace_kmer->assemblyInfo_.seq_num_<<" unitigs reported of length "<<work_queue4trace_kmer->assemblyInfo_.seq_len_in_total_<<" bp in total"<<endl;
    //cout<<"[Unitig] Among them, "<<duplicateUnitig_num<<" duplicate unitigs found of length "<<duplicateUnitigs_len<<" bp in total."<<endl;
    //cout<<"[Unitig] After removing duplicates, we have "<<noduplicate_contig_num<<" unitigs of length "<<totalUnitigs_len- duplicateUnitigs_len<<" bp in total."<<endl;
    //cout<<"[Unitig] Among all non-duplicate unitigs, there are "<<palindrome_contig_num<<" palindromes among "<<hairpin_contig_num+palindrome_contig_num<<" hairpins."<<endl;
    cout<<"[Unitig] among them, there are "<<work_queue4trace_kmer->assemblyInfo_.palindrome_seq_num_<<" palindromes."<<endl;
    free(work_queue4trace_kmer);
  //}
  //*/
  //return 0;

  //check whether can reconstruct the full sequences from the graph.
  // if(true){
  //   string refFile = "/public/hshi/tools/SH-assembly/test/case1/genome10K.fasta";
  //   ifstream refFin;
  //   refFin.open(refFile, ios::in);
  //   string refSeq="";
  //   getline(refFin, line);
  //   while(getline(refFin, line)){
  //     refSeq += line;
  //   }
  //   to_upper_DNA(refSeq);
  //   cout<<"[Test] length of reference sequence: "<<refSeq.length()<<" bp."<<endl;
  //   int constructed_len=0;
  //   DNAString kmer=refSeq.substr(0,options.K);
  //   // while(true){
  //   //   auto it = startKmer2unitig.find(kmer);
  //   //   if(it == startKmer2unitig.end()){
  //   //     cout<<"[Test] kmer not found: "<<kmer<<endl;  
  //   //     if(constructed_len != 0){
  //   //       cout<<"[Test] last kmer in the traversed sequence: "<<refSeq.substr(constructed_len-1, options.K)<<endl;
  //   //       cout<<"[Test] traversal of reference seq broken at "<<constructed_len+options.K-1<<endl;
  //   //     }
  //   //     break;
  //   //   }
  //   //   //get the real ID
  //   //   auto id = abs(it->second);
  //   //   while(id2id_afterRemoveDuplicate[id] < abs(it->second)){
  //   //     id++;
  //   //   }
  //   //   constructed_len += (contigs[id].seq.length() - options.K +1);
  //   //   cout<<"[Test] find kmer: "<<kmer<<" in contig "<<it->second<<" of length "<<contigs[id].seq.length()<<" bp."<<endl;
  //   //   cout<<"[Test] contig "<< it->second<<": ";
  //   //   if(it->second < 0)
  //   //     cout<<contigs[id].seq.get_RC();
  //   //   else
  //   //     cout<<contigs[id].seq;
  //   //   cout<<" median_abundance: "<<contigs[id].median_abundance<<endl;
  //   //   kmer = refSeq.substr(constructed_len, options.K);
  //   //   if(constructed_len+options.K-1 >= refSeq.length()){
  //   //     cout<<"[Test] reference seq is encoded in the DBG of unitigs"<<endl;
  //   //     break;
  //   //   }
  //   // }

  //   //check whether k-mer are in the CQF with proper coverage
  //   kmer = refSeq.substr(constructed_len, options.K);
  //   uint64_t kmer_hash, kmer_RC_hash, kmer_count;
  //   kmer_hash = NTPC64(kmer, options.K, kmer_hash, kmer_RC_hash);
  //   kmer_count = cqf_mt.count(kmer_hash%cqf_mt.qf->metadata->range);
  //   cout<<"[Test] count: "<<kmer_count<<" of kmer: "<<kmer<<endl;
  // }
  
  //output all kmers
  // for(auto ele : startKmer2unitig){
  //   cout<<ele.first<<":"<<ele.second<<endl;
  // }
  // for(int contig_id = 1; contig_id < 2; contig_id++){
  //   cout<<"Contig "<<contig_id<<":"<<contigs[contig_id].seq<<endl;
  //   if(isDuplicate[contig_id]){
  //     continue;
  //   }
  //   DNAString first_kmer = contigs[contig_id].seq.substr(0,options.K);
  //   DNAString last_kmer_RC = contigs[contig_id].seq.substr(contigs[contig_id].seq.length()-options.K).RC();
  //   if(startKmer2unitig.find(first_kmer) == startKmer2unitig.end()){
  //     cerr<<"[error] why not found: "<<first_kmer<<endl;
  //     for(auto ele : startKmer2unitig){
  //       if(ele.first == first_kmer){
  //         cerr<<"[error] Hmmm..."<<endl;
  //       }
  //     }
  //   }else{
  //     cerr<<"[error] "<<startKmer2unitig.find(first_kmer)->first<<endl;
  //   }
  //   if(startKmer2unitig.find(last_kmer_RC) == startKmer2unitig.end()){
  //     cerr<<"[error] why not found!"<<endl;
  //   }
  // }

  //output graph
  /*
  if(false){
    ofstream fout;
    string header, seq;
    DNAString kmer_fix, kmer;
    char bases[4]={'A','C','G','T'};
    fout.open(options.output, ios::out);
    for(int contig_id = 1; contig_id<contigs.size(); contig_id++){
      if(isDuplicate[contig_id]){
        continue;
      }
      fout<<">"<<(id2id_afterRemoveDuplicate[contig_id]-1)<<" LN:i:"<<contigs[contig_id].seq.length()<<" KC:i:"<<contigs[contig_id].median_abundance*(contigs[contig_id].seq.length()-options.K+1)<<" km:f:"<<contigs[contig_id].median_abundance;
      kmer_fix=contigs[contig_id].seq.substr(contigs[contig_id].seq.length()-options.K+1, options.K-1);

      for(int x=0; x<4; x++){
        kmer = kmer_fix; kmer.append(bases[x]);
        auto it = startKmer2unitig.find(kmer);
        if(it != startKmer2unitig.end()){
          auto tmp = it->second;
          if(tmp>0){
            fout<<" L:+:"<<tmp-1<<":+";
            if(isHairpin[tmp]){
              fout<<" L:+:"<<tmp-1<<":-";
            }
          }else{
            fout<<" L:+:"<<-tmp-1<<":-";
          }
        }else{
          continue;
        }
      }
      kmer_fix=contigs[contig_id].seq.substr(0, options.K-1).RC();
      for(int x=0; x<4; x++){
        kmer = kmer_fix; kmer.append(bases[x]);
        auto it = startKmer2unitig.find(kmer);
        if(it != startKmer2unitig.end()){
          auto tmp = it->second;
          if(tmp>0){
            fout<<" L:-:"<<tmp-1<<":+";
            if(isHairpin[tmp]){
              fout<<" L:-:"<<tmp-1<<":-";
            }
          }else{
            fout<<" L:-:"<<-tmp-1<<":-";
          }
        }else{
          continue;
        }
      }
      fout<<endl;
      fout<<contigs[contig_id].seq<<endl;
    }
    fout.close();
  }
  */
  //if(false){
    cout<<"[Unitig] build unitig graph."<<endl;
    vector<UnitigNode> unitigNodes(work_queue4trace_kmer->assemblyInfo_.seq_num_);//store the connection info of each node
    //string header, seq;
    //DNAString kmer_fix, kmer;
    //char bases[4]={'A','C','G','T'};  
    WorkQueue2* work_queue4build_graph = new WorkQueue2(0, 10, 0);
    for(int t = 0; t<options.thread_num; t++){
      prod_threads.add_thread(new boost::thread(build_graph_worker, boost::ref(options), boost::ref(contigs), boost::ref(startKmer2unitig), boost::ref(unitigNodes), work_queue4build_graph));
    }
    prod_threads.join_all();
    free(work_queue4build_graph);

    ofstream fout;
    fout.open(options.output, ios::out);
    size_t noduplicate_contig_num=0;
    for(int contig_id = 1; contig_id<contigs.size(); contig_id++){
      if(contigs[contig_id].seq.dna_base_num()==0){
        continue;
      }
      fout<<">"<<noduplicate_contig_num<<" LN:i:"<<contigs[contig_id].seq.length()<<" KC:i:"<<contigs[contig_id].median_abundance*(contigs[contig_id].seq.length()-options.K+1)<<" km:f:"<<contigs[contig_id].median_abundance;
      for(auto tmp : unitigNodes[noduplicate_contig_num].afterNodes){
        if(tmp>0){
          fout<<" L:+:"<<tmp-1<<":+";
        }else{
          fout<<" L:+:"<<-tmp-1<<":-";
        }
      }
      for(auto tmp : unitigNodes[noduplicate_contig_num].beforeNodes){
        if(tmp>0){
          fout<<" L:-:"<<tmp-1<<":+";
        }else{
          fout<<" L:-:"<<-tmp-1<<":-";
        }
      }
      fout<<endl<<contigs[contig_id].seq<<endl;
      noduplicate_contig_num++;
    }
    fout.close();
    
    /*
    size_t noduplicate_contig_num=0;
    for(int contig_id = 1; contig_id<contigs.size(); contig_id++){
      if(contigs[contig_id].seq.dna_base_num()==0){
        continue;
      }
      fout<<">"<<noduplicate_contig_num<<" LN:i:"<<contigs[contig_id].seq.length()<<" KC:i:"<<contigs[contig_id].median_abundance*(contigs[contig_id].seq.length()-options.K+1)<<" km:f:"<<contigs[contig_id].median_abundance;
      kmer_fix=contigs[contig_id].seq.substr(contigs[contig_id].seq.length()-options.K+1, options.K-1);

      for(int x=0; x<4; x++){
        kmer = kmer_fix; kmer.append(bases[x]);
        hash_map_mt::const_accessor const_access;
        if(startKmer2unitig.find(const_access, kmer)){
          auto tmp = const_access->second;
          if(tmp>0){
            fout<<" L:+:"<<tmp-1<<":+";
            //if(isHairpin[tmp]){
            //  fout<<" L:+:"<<tmp-1<<":-";
            //}
          }else{
            fout<<" L:+:"<<-tmp-1<<":-";
          }
        }
        const_access.release();
      }
      kmer_fix=contigs[contig_id].seq.substr(0, options.K-1).RC();
      for(int x=0; x<4; x++){
        kmer = kmer_fix; kmer.append(bases[x]);
        hash_map_mt::const_accessor const_access;
        if(startKmer2unitig.find(const_access, kmer)){
          auto tmp = const_access->second;
          if(tmp>0){
            fout<<" L:-:"<<tmp-1<<":+";
            //if(isHairpin[tmp]){
            //  fout<<" L:-:"<<tmp-1<<":-";
            //}
          }else{
            fout<<" L:-:"<<-tmp-1<<":-";
          }
        }
        const_access.release();
      }
      fout<<endl;
      fout<<contigs[contig_id].seq<<endl;
      noduplicate_contig_num++;
    }
    */
  //}

  DisplayCurrentDateTime();  
  //contig_summary(contigs);
  return 0;
}

//use 1-based counter, e.g. the first work has work_id 1.
/*
struct WorkQueue{
  volatile atomic<uint32_t> next_work;
  volatile atomic<uint32_t> total_work;
  volatile atomic<uint32_t> work_done;
  volatile atomic<bool> master_done;
  boost::mutex mut;

  WorkQueue(){
    next_work = 1;
    total_work = 0;
    work_done = 0;
    master_done = false;
  }
  WorkQueue(uint32_t n, uint32_t t){
    assert(n>0 && t>0);
    next_work = n;
    total_work = t;
    work_done = n-1;
    master_done = false;
  }
  
  bool get_next_work(uint32_t& work_id){
    boost::unique_lock<boost::mutex> lock(mut);
    if(next_work <= total_work){
      work_id = next_work;
      next_work++; 
      lock.unlock();
      return true;
    }else{
      lock.unlock();
      return false;
    }
  }
  void report_work_done(int num=1){
    work_done+=num;
  }

  void add_work(uint32_t work_num){
    total_work += work_num;
  }
  void add_skip_work(uint32_t work_num){
    boost::unique_lock<boost::mutex> lock(mut);
    next_work += work_num;
    total_work += work_num;
    work_done += work_num;
    lock.unlock();
  }
};
*/

struct WorkQueue{
  concurrent_queue<concurrent_vector<Contig>::iterator> jobQueue;
  volatile atomic<uint32_t> total_work;
  volatile atomic<uint32_t> work_done;
  volatile atomic<bool> master_done;
  //boost::mutex mut;

  WorkQueue(){
    total_work = 0;
    work_done = 0;
    master_done = false;
  }
  //WorkQueue(uint32_t t){
  //  assert(t>0);
    //next_work = n;
  //   total_work = t;
  //   work_done = ;
  //   master_done = false;
  // }
  
  bool get_next_work(concurrent_vector<Contig>::iterator& jobIter){
    if(jobQueue.try_pop(jobIter)){
      return true;
    }else{
      return false;
    }
  }
  void report_work_done(int num=1){
    work_done+=num;
  }

  void add_work(concurrent_vector<Contig>::iterator jobIter){
    jobQueue.push(jobIter);
    total_work ++;
  }
};

void track_kmer_worker(const Params& options, const concurrent_vector<Contig>& contigs, hash_map_mt& startKmer2unitig, WorkQueue2* work_queue){
  int start, end, counter, totalUnitigs_len, seq_num, palindrome_contig_num;
  totalUnitigs_len = seq_num = palindrome_contig_num = 0;
  DNAString first_kmer,last_kmer_RC;
  while(work_queue->get_work(contigs, start, end, counter)){
    for(size_t contig_id = start; contig_id < end; contig_id++){
      if(contigs[contig_id].seq.dna_base_num()!=0){
        seq_num++;
        totalUnitigs_len += contigs[contig_id].seq.length();
        first_kmer = contigs[contig_id].seq.substr(0,options.K);
        last_kmer_RC = contigs[contig_id].seq.substr(contigs[contig_id].seq.length()-options.K).RC();
        
        if(first_kmer == last_kmer_RC){
          palindrome_contig_num++;
          //startKmer2unitig[first_kmer] = noduplicate_contig_num;
          hash_map_mt::accessor access;
          if(startKmer2unitig.find(access, first_kmer)){
            access->second = counter;
          }else{
            cerr<<"[Error] kmer not found!"<<endl;
          }
          access.release();
          //dont't know how to do  
          //cout<<"[Warning] contig "<<contig_id<<" seems like a palindrome seq (+ and - strand): "<<contigs[contig_id].seq<<endl;
        }else{
          hash_map_mt::accessor access;
          if(startKmer2unitig.find(access, first_kmer)){
            access->second = counter;
          }else{
            cerr<<"[Error] kmer not found!"<<endl;
          }
          access.release();
          if(startKmer2unitig.find(access, last_kmer_RC)){
            access->second = -counter;
          }else{
            cerr<<"[Error] kmer not found!"<<endl;
          }
          access.release();
          //startKmer2unitig[first_kmer] = noduplicate_contig_num;
          //startKmer2unitig[last_kmer_RC] = -noduplicate_contig_num;
        }
        counter++;
      }
    }
  }
  work_queue->assemblyInfo_.seq_num_ += seq_num;
  work_queue->assemblyInfo_.palindrome_seq_num_ += palindrome_contig_num;
  work_queue->assemblyInfo_.seq_len_in_total_ += totalUnitigs_len;
}

void build_graph_worker(const Params& options, const concurrent_vector<Contig>& contigs, const hash_map_mt& startKmer2unitig, vector<UnitigNode>& unitigNodes, WorkQueue2* work_queue){
  DNAString kmer_fix, kmer;
  char bases[4]={'A','C','G','T'};
  int start, end, counter;//, first_count;
  while(work_queue->get_work(contigs, start, end, counter)){
    //first_count = counter; //mark the start of the counter.
    //stringstream buf;
    for(int contig_id = start; contig_id<end; contig_id++){
      if(contigs[contig_id].seq.dna_base_num()==0){
        continue;
      }
      //buf<<">"<<counter-1<<" LN:i:"<<contigs[contig_id].seq.length()<<" KC:i:"<<contigs[contig_id].median_abundance*(contigs[contig_id].seq.length()-options.K+1)<<" km:f:"<<contigs[contig_id].median_abundance;
      kmer_fix=contigs[contig_id].seq.substr(contigs[contig_id].seq.length()-options.K+1, options.K-1);

      for(int x=0; x<4; x++){
        kmer = kmer_fix; kmer.append(bases[x]);
        hash_map_mt::const_accessor const_access;
        if(startKmer2unitig.find(const_access, kmer)){
          auto tmp = const_access->second;
          unitigNodes[counter].afterNodes.push_back(tmp);
          // if(tmp>0){
          //   buf<<" L:+:"<<tmp-1<<":+";
          //   //if(isHairpin[tmp]){
          //   //  fout<<" L:+:"<<tmp-1<<":-";
          //   //}
          // }else{
          //   buf<<" L:+:"<<-tmp-1<<":-";
          // }
        }
        const_access.release();
      }
      kmer_fix=contigs[contig_id].seq.substr(0, options.K-1).RC();
      for(int x=0; x<4; x++){
        kmer = kmer_fix; kmer.append(bases[x]);
        hash_map_mt::const_accessor const_access;
        if(startKmer2unitig.find(const_access, kmer)){
          auto tmp = const_access->second;
          unitigNodes[counter].beforeNodes.push_back(tmp);
          // if(tmp>0){
          //   buf<<" L:-:"<<tmp-1<<":+";
          //   //if(isHairpin[tmp]){
          //   //  fout<<" L:-:"<<tmp-1<<":-";
          //   //}
          // }else{
          //   buf<<" L:-:"<<-tmp-1<<":-";
          // }
        }
        const_access.release();
      }
      //buf<<endl<<contigs[contig_id].seq<<endl;
      counter++;
    }
    // while(work_queue->count2output.front() != first_count){
    //   boost::this_thread::sleep_for(boost::chrono::milliseconds(20));
    // }
    // work_queue->write_mut_.lock();
    // fout<<buf.str();
    // work_queue->count2output.pop();
    // work_queue->write_mut_.unlock();
  }
}

/*
void find_unitigs_mt_master(CQF_mt& cqf, const vector<string>& seqFiles, const Params& params, vector_mt<string>& contigs){
  //initialized worker, assume contigs.size()>=1
  WorkQueue* work_queue = new WorkQueue(contigs.size(), contigs.size());
  unordered_map_mt<string, int> startKmer2unitig(1000);

  boost::thread_group prod_threads;
  for(int t = 0; t<params.thread_num; t++){
    prod_threads.add_thread(new boost::thread(find_unitigs_mt_worker, boost::ref(cqf), boost::ref(params), boost::ref(contigs), boost::ref(startKmer2unitig), work_queue));
  }
  
  string kmer, kmer_RC;
  uint64_t kmer_hash, kmer_RC_hash;
  uint64_t kmer_count, kmer_RC_count;
  size_t contig_id;
  
  for(auto file : seqFiles){
    cout<<"Processing "<<file<<endl;
    std::ifstream fin(file, std::ios_base::in | std::ios_base::binary);
    boost::iostreams::filtering_streambuf<boost::iostreams::input> inbuf;
    inbuf.push(boost::iostreams::gzip_decompressor());
    inbuf.push(fin);
    //Convert streambuf to istream
    std::istream instream(&inbuf);
    //Iterate lines
    std::string line, seq;
    while(std::getline(instream, line)){
      //skip the header line
      if(line.empty()){
        continue;
      }else if(line[0] != '@'){
        continue;
      }
      if(!std::getline(instream, seq)){
        break;
      }
      //cout<<seq<<endl;
      if(seq.length()<params.K){
        continue;
      }
      //seq_RC = RC_DNA(seq);
      int seq_len = seq.length();
      int step = seq_len/3;
      for(int x = 0; x<=seq_len-params.K; x += step){
        kmer = seq.substr(x, params.K);
        kmer_RC = RC_DNA(kmer);//seq_RC.substr(seq_len-params.K-x, params.K);
        if(kmer.find_first_of("nN")!=string::npos){
          continue;
        }
        //kmer_hash = MurmurHash2(kmer);
        //kmer_RC_hash = MurmurHash2(kmer_RC);
        NTPC64(kmer.c_str(), params.K, kmer_hash, kmer_RC_hash);
        
        if(cqf.count_key_value_set_traveled(kmer_hash%cqf.qf->metadata->range, kmer_count) && cqf.count_key_value_set_traveled(kmer_RC_hash%cqf.qf->metadata->range, kmer_RC_count)){
          continue;
        }else if(kmer_count < params.CONTIG_min_cov || kmer_RC_count < params.CONTIG_min_cov){
          continue;
        }

        contig_id = contigs.size_mt();
        startKmer2unitig.insert_mt(kmer, contig_id);
        startKmer2unitig.insert_mt(kmer_RC, -contig_id);
        work_queue->add_skip_work(1);    
        contigs.push_back_mt(seq);

        get_unitig_forward(cqf, params, contigs, startKmer2unitig, work_queue, contig_id);
        get_unitig_backward(cqf, params, contigs, startKmer2unitig, work_queue, contig_id);

        //wait for the end of this run.
        while(work_queue->next_work != work_queue->total_work){
          std::this_thread::sleep_for(std::chrono::milliseconds(1000));//sleep for 1 seconds  
        }
      }
      //skip two lines
      std::getline(instream, line);
      std::getline(instream, line);
    }
  }
  work_queue->master_done = true;
  prod_threads.join_all();
}
*/
/*
void find_unitigs_mt_master(CQF_mt& cqf, seqFile_batch& seqFiles, const Params& options, vector_mt<Contig>& contigs){
  WorkQueue* work_queue = new WorkQueue();
  unordered_map_mt<string, int> startKmer2unitig(1000);

  boost::thread_group prod_threads;
  for(int t = 0; t<options.thread_num; t++){
    prod_threads.add_thread(new boost::thread(find_unitigs_mt_worker, boost::ref(cqf), boost::ref(options), boost::ref(contigs), boost::ref(startKmer2unitig), work_queue));
  }
  
  string kmer, kmer_RC;
  uint64_t kmer_hash, kmer_RC_hash;
  uint64_t kmer_count, kmer_RC_count;
  size_t contig_id;
  
  vector<Contig> master_contigs;
  chunk dataChunk;
  while(seqFiles.getDataChunk(dataChunk)){
    std::string line, seq;
    while(dataChunk.readLine(line)){
      //skip the header line
      if(line.empty()){
        continue;
      }else if(line[0] != '@'){
        continue;
      }
      if(!dataChunk.readLine(seq)){
        break;
      }
      //cout<<seq<<endl;
      if(seq.length()<options.K){
        continue;
      }
      //seq_RC = RC_DNA(seq);
      int seq_len = seq.length();
      int step = seq_len/3;
      for(int x = 0; x<=seq_len- options.K; x += step){
        kmer = seq.substr(x, options.K);
        kmer = to_upper_DNA(kmer);
        //kmer_RC = RC_DNA(kmer);//seq_RC.substr(seq_len-params.K-x, params.K);
        if(kmer.find_first_of("nN")!=string::npos){
          continue;
        }
        kmer_hash = NTPC64(kmer.c_str(), options.K, kmer_hash, kmer_RC_hash);

        if(cqf.count_key_value_set_traveled(kmer_hash%cqf.qf->metadata->range, kmer_count)){
          continue;
        }else if(kmer_count < options.kmer_abundance_min){
          continue;
        }

        //contig_id = contigs.push_back_mt(Contig(kmer, kmer_count));
        //work_queue->add_skip_work(1);
        //startKmer2unitig.insert_mt(kmer, contig_id); //may not be the start k-mer
        startKmer2unitig.insert_mt(kmer, 0);//fake id 
        //startKmer2unitig.insert_mt(kmer_RC, -contig_id); //may not be the end k-mer
        Contig master_contig(kmer, kmer_count);
        get_unitig_forward(cqf, options, contigs, startKmer2unitig, work_queue, master_contig);
        master_contig.seq = RC_DNA(master_contig.seq);
        get_unitig_forward(cqf, options, contigs, startKmer2unitig, work_queue, master_contig);
        master_contigs.push_back(master_contig);
        //std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        break;
        //get_unitig_forward(cqf, options, contigs, startKmer2unitig, work_queue, contig_id);
        //contigs.set_mt(contig_id, Contig(RC_DNA(contigs[contig_id].seq), contigs[contig_id].median_abundance));
        //get_unitig_forward(cqf, options, contigs, startKmer2unitig, work_queue, contig_id);
        //get_unitig_backward(cqf, options, contigs, startKmer2unitig, work_queue, contig_id);

        //wait for the end of this run.
        //while(work_queue->work_done != work_queue->total_work){
        //  std::this_thread::sleep_for(std::chrono::milliseconds(1000));//sleep for 1 seconds  
        //}
      }
      //skip two lines
      dataChunk.skipLines(2);
    }
    //if load detected is less than half. start processing a new chunk
    while(work_queue->work_done != work_queue->total_work){
    //while(work_queue->total_work/work_queue->work_done) > ){
      if(work_queue->next_work > work_queue->total_work){
        cout<<"[test] stucked.."<<endl;
      }
      cout<<"[test] "<<work_queue->next_work<<":"<<work_queue->work_done<<":"<<work_queue->total_work<<endl;
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));//sleep for 1 seconds  
    }
  }
  work_queue->master_done = true;
  prod_threads.join_all();

  contigs.insert(contigs.end(), master_contigs.begin(), master_contigs.end());
}
*/
/*
void processDataChunk(CQF_mt& cqf, const Params& options, concurrent_vector<Contig>& contigs, unordered_set_mt& startKmers, WorkQueue* work_queue, chunk& dataChunk){
  string kmer;
  uint64_t kmer_hash, kmer_RC_hash;
  uint64_t kmer_count, kmer_RC_count;
  std::string line, seq;
  while(dataChunk.readLine(line)){
    //skip the header line
    if(line.empty()){
      continue;
    }else if(line[0] != '@'){
      continue;
    }
    if(!dataChunk.readLine(seq)){
      break;
    }
    if(seq.length()<options.K){
      dataChunk.skipLines(2);//skip two lines after the sequence line
      continue;
    }

    int seq_len = seq.length();
    int middle = seq_len/2;

    if(middle <= seq_len- options.K){
      kmer = seq.substr(middle, options.K);
      if(kmer.find_first_of("nN")!=string::npos){
        continue;
      }
      kmer_hash = NTPC64(kmer.c_str(), options.K, kmer_hash, kmer_RC_hash);

      if(cqf.count_key_value_set_traveled(kmer_hash%cqf.qf->metadata->range, kmer_count)){
        continue;
      }else if(kmer_count < options.solid_kmer_abundance_min || kmer_count > options.solid_kmer_abundance_max){
        continue;
      }

      auto iter = contigs.push_back(Contig(kmer, kmer_count));
      to_upper_DNA(kmer);
      startKmers.insert(kmer);//fake id 

      get_unitig_forward(cqf, options, contigs, startKmers, work_queue, iter);
      iter->seq.RC();
      get_unitig_forward(cqf, options, contigs, startKmers, work_queue, iter);
    }
    //skip two lines
    dataChunk.skipLines(2);
  }
  //free data chunk
  free(dataChunk.get_reads());
}
*/

void processDataChunk(CQF_mt& cqf, const Params& options, concurrent_vector<Contig>& contigs, hash_map_mt& startKmer2unitig, WorkQueue* work_queue, chunk& dataChunk){
  string kmer;
  uint64_t kmer_hash, kmer_RC_hash;
  uint64_t kmer_count, kmer_RC_count;
  std::string line, seq;
  while(dataChunk.readLine(line)){
    //skip the header line
    if(line.empty()){
      continue;
    }else if(line[0] != '@'){
      continue;
    }
    if(!dataChunk.readLine(seq)){
      break;
    }
    if(seq.length()<options.K){
      dataChunk.skipLines(2);//skip two lines after the sequence line
      continue;
    }

    int seq_len = seq.length();
    int middle = seq_len/2;

    if(middle <= seq_len- options.K){
      kmer = seq.substr(middle, options.K);
      if(kmer.find_first_of("nN")!=string::npos){
        continue;
      }
      kmer_hash = NTPC64(kmer.c_str(), options.K, kmer_hash, kmer_RC_hash);

      if(cqf.count_key_value_set_traveled(kmer_hash%cqf.qf->metadata->range, kmer_count)){
        continue;
      }else if(kmer_count < options.solid_kmer_abundance_min || kmer_count > options.solid_kmer_abundance_max){
        continue;
      }

      auto iter = contigs.push_back(Contig(kmer, kmer_count));
      to_upper_DNA(kmer);
      
      size_t contig_id = iter - contigs.begin(); //std::distance(contigs.begin(), iter);
      bool is_dup = false;
      for(int x = 0; x<4; x++){
        if(kmer == string(options.K, DNA::bases[x])){
          hash_map_mt::accessor access;
          if(startKmer2unitig.insert(access, DNAString(kmer))){
            access->second = contig_id;
          }else{
            is_dup = true;
          }
          break;
        }
      }
      //startKmers.insert(DNAString(kmer));//fake id 
      if(is_dup){
        continue;
      }

      get_unitig_forward(cqf, options, contigs, startKmer2unitig, work_queue, iter);
      if(iter->seq.dna_base_num() > 0){
        iter->seq.RC() ;
        get_unitig_forward(cqf, options, contigs, startKmer2unitig, work_queue, iter);
      }
    }
    //skip two lines
    dataChunk.skipLines(2);
  }
  //free data chunk
  free(dataChunk.get_reads());
}
/*
void find_unitigs_mt_master(CQF_mt& cqf, seqFile_batch& seqFiles, const Params& options, concurrent_vector<Contig>& contigs){
  WorkQueue* work_queue = new WorkQueue();
  unordered_set_mt startKmers;

  boost::thread_group prod_threads;
  for(int t = 0; t<options.thread_num; t++){
    prod_threads.add_thread(new boost::thread(find_unitigs_mt_worker, boost::ref(cqf), boost::ref(seqFiles), boost::ref(options), boost::ref(contigs), boost::ref(startKmers), work_queue));
  }
  
  string kmer, kmer_RC;
  uint64_t kmer_hash, kmer_RC_hash;
  uint64_t kmer_count, kmer_RC_count;
  //size_t contig_id;
  
  //vector<Contig> master_contigs;
  chunk dataChunk;
  while(seqFiles.getDataChunk(dataChunk)){
    std::string line, seq;
    while(dataChunk.readLine(line)){
      //skip the header line
      if(line.empty()){
        continue;
      }else if(line[0] != '@'){
        continue;
      }
      if(!dataChunk.readLine(seq)){
        break;
      }
      //cout<<seq<<endl;
      if(seq.length()<options.K){
        dataChunk.skipLines(2);
        continue;
      }
      //seq_RC = RC_DNA(seq);
      int seq_len = seq.length();
      int middle = seq_len/2;

      if(middle <= seq_len- options.K){
        kmer = seq.substr(middle, options.K);
        //kmer = 
        to_upper_DNA(kmer);
        //kmer_RC = RC_DNA(kmer);//seq_RC.substr(seq_len-params.K-x, params.K);
        if(kmer.find_first_of("nN")!=string::npos){
          continue;
        }
        kmer_hash = NTPC64(kmer.c_str(), options.K, kmer_hash, kmer_RC_hash);

        if(cqf.count_key_value_set_traveled(kmer_hash%cqf.qf->metadata->range, kmer_count)){
          continue;
        }else if(kmer_count < options.solid_kmer_abundance_min || kmer_count > options.solid_kmer_abundance_max){
          continue;
        }

        //contig_id = contigs.push_back_mt(Contig(kmer, kmer_count));
        //work_queue->add_skip_work(1);
        auto iter = contigs.push_back(Contig(kmer, kmer_count));
        //startKmer2unitig.insert_mt(kmer, contig_id); //may not be the start k-mer
        startKmers.insert(DNAString(kmer));//fake id 
        //startKmer2unitig.insert_mt(kmer_RC, -contig_id); //may not be the end k-mer
        // Contig master_contig(kmer, kmer_count);
        // get_unitig_forward(cqf, options, contigs, startKmer2unitig, work_queue, master_contig);
        // master_contig.seq = RC_DNA(master_contig.seq);
        // get_unitig_forward(cqf, options, contigs, startKmer2unitig, work_queue, master_contig);
        // master_contigs.push_back(master_contig);
        //std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        //break;
        get_unitig_forward(cqf, options, contigs, startKmers, work_queue, iter);
        //contigs.set(contig_id, Contig(RC_DNA(contigs[contig_id].seq), contigs[contig_id].median_abundance));
        iter->seq.RC();
        get_unitig_forward(cqf, options, contigs, startKmers, work_queue, iter);
        //get_unitig_backward(cqf, options, contigs, startKmer2unitig, work_queue, contig_id);

        //std::this_thread::sleep_for(std::chrono::milliseconds(300));
        //wait for the end of this run.
      }
      //skip two lines
      dataChunk.skipLines(2);
      //auto tmp_total = work_queue->total_work;
      //auto tmp_done = work_queue->work_done;
      while((work_queue->total_work - work_queue->work_done) > options.thread_num){
        //cout<<" "<<work_queue->work_done<<"/"<<work_queue->total_work<<"("<<work_queue->total_work-work_queue->work_done<<")"<<contigs.size()<<std::flush;
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));//sleep for 1 seconds  
      }
    }
    //free data chunk
    free(dataChunk.get_reads());

    //if load detected is less than half. start processing a new chunk
    //while(work_queue->work_done != work_queue->total_work){
    //while(work_queue->total_work/work_queue->work_done) > ){
      //if(work_queue->next_work > work_queue->total_work){
      //  cout<<"[test] stucked.."<<endl;
      //}
      //cout<<"[test] "<<work_queue->next_work<<":"<<work_queue->work_done<<":"<<work_queue->total_work<<endl;
      //std::this_thread::sleep_for(std::chrono::milliseconds(1000));//sleep for 1 seconds  
    //}
  }
  while(work_queue->total_work > work_queue->work_done){
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  }
  work_queue->master_done = true;
  prod_threads.join_all();

  //contigs.insert(contigs.end(), master_contigs.begin(), master_contigs.end());
  startKmers.clear();
}
*/

void find_unitigs_mt_master(CQF_mt& cqf, seqFile_batch& seqFiles, const Params& options, concurrent_vector<Contig>& contigs, hash_map_mt& startKmer2unitig){
  WorkQueue* work_queue = new WorkQueue();

  boost::thread_group prod_threads;
  for(int t = 0; t<options.thread_num; t++){
    prod_threads.add_thread(new boost::thread(find_unitigs_mt_worker, boost::ref(cqf), boost::ref(seqFiles), boost::ref(options), boost::ref(contigs), boost::ref(startKmer2unitig), work_queue));
  }
  
  string kmer, kmer_RC;
  uint64_t kmer_hash, kmer_RC_hash;
  uint64_t kmer_count, kmer_RC_count;
  size_t contig_id;
  
  //vector<Contig> master_contigs;
  chunk dataChunk;
  while(seqFiles.getDataChunk(dataChunk)){
    std::string line, seq;
    while(dataChunk.readLine(line)){
      //skip the header line
      if(line.empty()){
        continue;
      }else if(line[0] != '@'){
        continue;
      }
      if(!dataChunk.readLine(seq)){
        break;
      }
      //cout<<seq<<endl;
      if(seq.length()<options.K){
        dataChunk.skipLines(2);
        continue;
      }
      //seq_RC = RC_DNA(seq);
      int seq_len = seq.length();
      int middle = seq_len/2;

      if(middle <= seq_len- options.K){
        kmer = seq.substr(middle, options.K);
        //kmer = 
        to_upper_DNA(kmer);
        //kmer_RC = RC_DNA(kmer);//seq_RC.substr(seq_len-params.K-x, params.K);
        if(kmer.find_first_of("nN")!=string::npos){
          continue;
        }
        kmer_hash = NTPC64(kmer.c_str(), options.K, kmer_hash, kmer_RC_hash);

        if(cqf.count_key_value_set_traveled(kmer_hash%cqf.qf->metadata->range, kmer_count)){
          continue;
        }else if(kmer_count < options.solid_kmer_abundance_min || kmer_count > options.solid_kmer_abundance_max){
          continue;
        }

        //contig_id = contigs.push_back_mt(Contig(kmer, kmer_count));
        //work_queue->add_skip_work(1);
        auto iter = contigs.push_back(Contig(kmer, kmer_count));
        contig_id = iter - contigs.begin(); //std::distance(contigs.begin(), iter);
        //startKmer2unitig.insert_mt(kmer, contig_id); //may not be the start k-mer
        bool is_dup = false;
        for(int x = 0; x<4; x++){
          if(kmer == string(options.K, DNA::bases[x])){
            hash_map_mt::accessor access;
            if(startKmer2unitig.insert(access, DNAString(kmer))){
              access->second = contig_id;
            }else{
              is_dup = true;
            }
            access.release();
            break;
          }
        }
        //startKmers.insert(DNAString(kmer));//fake id 
        if(is_dup){
          continue;
        }

        //startKmer2unitig.insert_mt(kmer_RC, -contig_id); //may not be the end k-mer
        // Contig master_contig(kmer, kmer_count);
        // get_unitig_forward(cqf, options, contigs, startKmer2unitig, work_queue, master_contig);
        // master_contig.seq = RC_DNA(master_contig.seq);
        // get_unitig_forward(cqf, options, contigs, startKmer2unitig, work_queue, master_contig);
        // master_contigs.push_back(master_contig);
        //std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        //break;
        get_unitig_forward(cqf, options, contigs, startKmer2unitig, work_queue, iter);
        //contigs.set(contig_id, Contig(RC_DNA(contigs[contig_id].seq), contigs[contig_id].median_abundance));
        if(iter->seq.dna_base_num() > 0){
          iter->seq.RC();
          get_unitig_forward(cqf, options, contigs, startKmer2unitig, work_queue, iter);
        }
        //get_unitig_backward(cqf, options, contigs, startKmer2unitig, work_queue, contig_id);

        //std::this_thread::sleep_for(std::chrono::milliseconds(300));
        //wait for the end of this run.
      }
      //skip two lines
      dataChunk.skipLines(2);
      //auto tmp_total = work_queue->total_work;
      //auto tmp_done = work_queue->work_done;
      while((work_queue->total_work - work_queue->work_done) > options.thread_num){
        //cout<<" "<<work_queue->work_done<<"/"<<work_queue->total_work<<"("<<work_queue->total_work-work_queue->work_done<<")"<<contigs.size()<<std::flush;
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));//sleep for 1 seconds  
      }
    }
    //free data chunk
    free(dataChunk.get_reads());

    //if load detected is less than half. start processing a new chunk
    //while(work_queue->work_done != work_queue->total_work){
    //while(work_queue->total_work/work_queue->work_done) > ){
      //if(work_queue->next_work > work_queue->total_work){
      //  cout<<"[test] stucked.."<<endl;
      //}
      //cout<<"[test] "<<work_queue->next_work<<":"<<work_queue->work_done<<":"<<work_queue->total_work<<endl;
      //std::this_thread::sleep_for(std::chrono::milliseconds(1000));//sleep for 1 seconds  
    //}
  }
  while(work_queue->total_work > work_queue->work_done){
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  }
  work_queue->master_done = true;
  prod_threads.join_all();

  free(work_queue);
  //contigs.insert(contigs.end(), master_contigs.begin(), master_contigs.end());
  //startKmer2unitig.clear();
}

/*
void find_unitigs_mt_worker(CQF_mt& cqf, const Params& params, vector_mt<string>& contigs, unordered_map_mt<string, int>& startKmer2unitig, WorkQueue* work_queue){
  uint32_t contig_id=0;
  while(!work_queue->master_done){
    if(!work_queue->get_next_work(contig_id)){
      std::this_thread::sleep_for(std::chrono::milliseconds(500));//sleep for 0.5 seconds
      continue;
    }
    string seq=contigs.at(contig_id);
    string kmer, kmer_RC;
    kmer = seq;
    kmer_RC = RC_DNA(kmer);
    string start_kmer = seq;
    int cid1, cid2;
    if(startKmer2unitig.find_mt(kmer, cid1)){
      if(startKmer2unitig.find_mt(kmer_RC, cid2)){
        if(cid1==contig_id && cid2==contig_id){//?
          get_unitig_forward(cqf, params, contigs, startKmer2unitig, work_queue, contig_id);
        }else if(cid1== -contig_id && cid2 == -contig_id){//?
          get_unitig_backward(cqf, params, contigs, startKmer2unitig, work_queue, contig_id);
        }
        continue;
      }
      if(cid1==contig_id){
        get_unitig_forward(cqf, params, contigs, startKmer2unitig, work_queue, contig_id);
        continue;
      }
    }else if(startKmer2unitig.find_mt(kmer_RC, cid2)){
      if(cid2==-contig_id){
        get_unitig_backward(cqf, params, contigs, startKmer2unitig, work_queue, contig_id);
      }
    }
  }
}
*/
/*
void find_unitigs_mt_worker(CQF_mt& cqf, const Params& options, vector_mt<Contig>& contigs, unordered_map_mt<string, int>& startKmer2unitig, WorkQueue* work_queue){
  uint32_t contig_id=0;
  while(!work_queue->master_done){
    if(!work_queue->get_next_work(contig_id)){
      std::this_thread::sleep_for(std::chrono::milliseconds(500));//sleep for 0.5 seconds
      continue;
    }
    get_unitig_forward(cqf, options, contigs, startKmer2unitig, work_queue, contig_id);
    work_queue->report_work_done();
  }
}
*/
/*
void find_unitigs_mt_worker(CQF_mt& cqf, const Params& options, vector_mt<Contig>& contigs, unordered_set_mt& startKmers, WorkQueue* work_queue){
  uint32_t contig_id=0;
  while(!work_queue->master_done){
    if(!work_queue->get_next_work(contig_id)){
      std::this_thread::sleep_for(std::chrono::milliseconds(500));//sleep for 0.5 seconds
      continue;
    }
    get_unitig_forward(cqf, options, contigs, startKmers, work_queue, contig_id);
    work_queue->report_work_done();
  }
}
*/
/*
void find_unitigs_mt_worker(CQF_mt& cqf, seqFile_batch& seqFiles, const Params& options, concurrent_vector<Contig>& contigs, unordered_set_mt& startKmers, WorkQueue* work_queue){
  concurrent_vector<Contig>::iterator contigIter;
  chunk dataChunk;
  while(!work_queue->master_done){
    if(!work_queue->get_next_work(contigIter)){
      if(seqFiles.getDataChunk(dataChunk)){
        processDataChunk(cqf, options, contigs, startKmers, work_queue, dataChunk);
      }else{
        std::this_thread::sleep_for(std::chrono::milliseconds(500));//sleep for 0.5 seconds
      }
    }else{
      get_unitig_forward(cqf, options, contigs, startKmers, work_queue, contigIter);
      work_queue->report_work_done();
    }
  }
}
*/

void find_unitigs_mt_worker(CQF_mt& cqf, seqFile_batch& seqFiles, const Params& options, concurrent_vector<Contig>& contigs, hash_map_mt& startKmer2unitig, WorkQueue* work_queue){
  concurrent_vector<Contig>::iterator contigIter;
  chunk dataChunk;
  while(!work_queue->master_done){
    if(!work_queue->get_next_work(contigIter)){
      if(seqFiles.getDataChunk(dataChunk)){
        processDataChunk(cqf, options, contigs, startKmer2unitig, work_queue, dataChunk);
      }else{
        std::this_thread::sleep_for(std::chrono::milliseconds(500));//sleep for 0.5 seconds
      }
    }else{
      get_unitig_forward(cqf, options, contigs, startKmer2unitig, work_queue, contigIter);
      work_queue->report_work_done();
    }
  }
}
/*
void get_unitig_forward(CQF_mt& cqf, const Params& params, vector_mt<string>& contigs, unordered_map_mt<string, int>& startKmer2unitig, WorkQueue* work_queue, int contig_id){
  array<bool, 4> candidates_before({false, false, false, false}), candidates_after({false, false, false, false});
  array<string, 4> kmer_befores, kmer_afters, kmer_befores_RC, kmer_afters_RC;
  int candidates_before_num, candidates_after_num;
  int nodes_before_num, nodes_after_num;
  uint64_t kmer_hash, kmer_RC_hash, current_kmer_hash, current_kmer_RC_hash;
  string kmer, kmer_RC, current_kmer;
  uint64_t kmer_count, kmer_RC_count;
  int idx;
  
  string contig_seq = contigs.at_mt(contig_id);
  current_kmer = contig_seq.substr(contig_seq.length()-params.K);

  while(true){
    kmer_afters = kmers_after(current_kmer);
    kmer_befores = kmers_before(kmer_afters[0]);
    candidates_before = candidates_after = {{false, false, false, false}};
    candidates_before_num = candidates_after_num = 0;
    nodes_before_num = nodes_after_num = 0;
    
    for(int x = 0; x<4; x++){
      kmer = kmer_afters[x];
      kmer_RC = RC_DNA(kmer);
      kmer_afters_RC[x] = kmer_RC;

      NTPC64(kmer.c_str(), params.K, kmer_hash, kmer_RC_hash);
      //kmer_hash = MurmurHash2(kmer);
      //kmer_RC_hash = MurmurHash2(kmer_RC);
      if(cqf.count_key_value_set_traveled(kmer_hash%cqf.qf->metadata->range, kmer_count) && cqf.count_key_value_set_traveled(kmer_RC_hash%cqf.qf->metadata->range, kmer_RC_count)){
        if(startKmer2unitig.find_mt(kmer, idx)){
          nodes_after_num++;
        }else if(kmer_count >= params.CONTIG_min_cov && kmer_RC_count >= params.CONTIG_min_cov){
          candidates_after[x] = true;
          candidates_after_num ++;
        }
      }else if(kmer_count >= params.CONTIG_min_cov && kmer_RC_count >= params.CONTIG_min_cov){
        candidates_after[x] = true;
        candidates_after_num ++;
      }
    }
    
    for(int x = 0; x<4; x++){
      if(kmer_befores[x] == current_kmer){
        continue;
      }
      kmer = kmer_befores[x];
      kmer_RC = RC_DNA(kmer);
      kmer_befores_RC[x] = kmer_RC;

      NTPC64(kmer.c_str(), params.K, kmer_hash, kmer_RC_hash);
      //kmer_hash = MurmurHash2(kmer);
      //kmer_RC_hash = MurmurHash2(kmer_RC);
      if(cqf.count_key_value_set_traveled(kmer_hash%cqf.qf->metadata->range, kmer_count) && cqf.count_key_value_set_traveled(kmer_RC_hash%cqf.qf->metadata->range, kmer_RC_count)){
        if(startKmer2unitig.find_mt(kmer_RC, idx)){
          nodes_before_num++;
        }else if(kmer_count >= params.CONTIG_min_cov && kmer_RC_count >= params.CONTIG_min_cov){
          candidates_before[x] = true;
          candidates_before_num++;
        }
      }else if(kmer_count >= params.CONTIG_min_cov && kmer_RC_count >= params.CONTIG_min_cov){
        candidates_before[x] = true;
        candidates_before_num++;
      }
    }
    if((nodes_before_num + candidates_before_num) || (nodes_after_num+candidates_after_num)>1){
      if(startKmer2unitig.find_mt(contig_seq.substr(0, params.K), idx)){
        //A contig has been constructed by another program from RC way
        if(abs(idx) != contig_id){
          contigs.set_mt(contig_id, ""); 
          break;
        }
      }else{
        throw logic_error("Unexpectedly not found start kmer!");
      }
      startKmer2unitig.insert_mt(RC_DNA(current_kmer), -contig_id);
      
      for(int x= 0; x < 4; x++){
        if(candidates_after[x]){
          if(!startKmer2unitig.find_mt(kmer_afters[x], idx)){
            int new_unitig_idx = contigs.push_back_mt(kmer_afters[x]);
            startKmer2unitig.insert_mt(kmer_afters[x], new_unitig_idx);
            work_queue->add_work(1); 
          }
        }
      }
      for(int x = 0; x < 4; x++){
        if(candidates_before[x]){
          if(!startKmer2unitig.find_mt(kmer_befores_RC[x], idx)){
            int new_unitig_idx = contigs.push_back_mt(kmer_befores_RC[x]);
            startKmer2unitig[kmer_befores_RC[x]] = -new_unitig_idx;
            work_queue->add_work(1);
          }
        }
      }
    }else if(candidates_after_num==1){
      for(int x = 0; x<4; x++){
        if(candidates_after[x]){
          current_kmer = kmer_afters[x];
          contig_seq += current_kmer.back();
          break;
        }
      }
      continue;
    }else{
      startKmer2unitig.insert_mt(RC_DNA(current_kmer), contig_id);
    }
    break;
  }
}
*/
/*
void get_unitig_forward(CQF_mt& cqf, const Params& options, vector_mt<Contig>& contigs, unordered_map_mt<string, int>& startKmer2unitig, WorkQueue* work_queue, const int& contig_id){
  array<bool, 4> candidates_before({false, false, false, false}), candidates_after({false, false, false, false});
  //array<string, 4> kmer_befores, kmer_afters, kmer_befores_RC, kmer_afters_RC;
  array<uint64_t, 4> kmer_abundance_befores, kmer_abundance_afters;
  auto abundance_min = options.kmer_abundance_min;
  auto K = options.K;

  int candidates_before_num, candidates_after_num;
  int nodes_before_num, nodes_after_num;
  uint64_t kmer_hash, kmer_RC_hash, current_kmer_hash, current_kmer_RC_hash;
  string kmer, kmer_RC, current_kmer, current_kmer_RC, current_kmer_fix;
  uint64_t kmer_count, kmer_RC_count;
  int idx;
  
  string contig_seq = contigs.at_mt(contig_id).seq;
  current_kmer = contig_seq.substr(contig_seq.length()-K);
  current_kmer_RC = RC_DNA(current_kmer);

  std::vector<int> abundances;
  abundances.push_back(int(contigs.at_mt(contig_id).median_abundance));

  NTPC64(current_kmer.c_str(), K, current_kmer_hash, current_kmer_RC_hash);
  int node_after_x, node_before_x;//useful only when there is only one node after and without candidates after.
  while(true){
    //NTPC64(current_kmer.c_str(), K, current_kmer_hash, current_kmer_RC_hash);
    current_kmer_fix = current_kmer.substr(1);
    //kmer_afters = kmers_after(current_kmer);
    //kmer_befores = kmers_before(kmer_afters[0]);
    candidates_before = candidates_after = {{false, false, false, false}};
    candidates_before_num = candidates_after_num = 0;
    nodes_before_num = nodes_after_num = 0;
    
    //kmers with current_kmer_fix as prefix
    for(int x = 0; x<4; x++){
      //kmer = kmer_afters[x];
      //kmer_RC = RC_DNA(kmer);
      //kmer_afters_RC[x] = kmer_RC;
      kmer_hash = current_kmer_hash; kmer_RC_hash = current_kmer_RC_hash;
      kmer_hash = NTPC64(current_kmer[0], DNA_bases[x], K, kmer_hash, kmer_RC_hash);
      //kmer_hash = MurmurHash2(kmer);
      //kmer_RC_hash = MurmurHash2(kmer_RC);
      if(cqf.count_key_value_set_traveled(kmer_hash%cqf.qf->metadata->range, kmer_count)){
        if(startKmer2unitig.find_mt(current_kmer_fix+DNA_bases[x], idx)){
          nodes_after_num++;
          kmer_abundance_afters[x] = kmer_count;
          node_after_x = x;
        }else if(kmer_count >= abundance_min){
          kmer_abundance_afters[x] = kmer_count;
          candidates_after[x] = true; //possible because of hash collisions
          candidates_after_num ++;
        }
      }else if(kmer_count >= abundance_min){
        kmer_abundance_afters[x] = kmer_count;
        candidates_after[x] = true;
        candidates_after_num ++;
      }
    }
    
    //kmers with RC(current_kmer_fix) as prefix
    NTPC64(current_kmer[0], 'A', options.K, current_kmer_hash, current_kmer_RC_hash);
    kmer = current_kmer_RC;
    for(int x = 0; x<4; x++){
      if(DNA_bases[x] == current_kmer_RC[K-1]){
        continue;
      }
      //kmer = kmer_befores[x];
      //kmer_RC = RC_DNA(kmer);
      //kmer_befores_RC[x] = kmer_RC;
      //kmer_hash = current_kmer_hash; kmer_RC_hash = current_kmer_RC_hash;
      //kmer_hash = NTPC64(current_kmer_RC[0], DNA_bases[x], K, kmer_hash, kmer_RC_hash);
      //kmer = current_kmer_RC;
      kmer[K-1] = DNA_bases[x];
      kmer_hash = current_kmer_hash; kmer_RC_hash = current_kmer_RC_hash;
      kmer_hash = NTPC64('T', DNA_bases[x], options.K, kmer_RC_hash, kmer_hash);
      //kmer_hash = NTPC64(kmer.c_str(), K, kmer_hash, kmer_RC_hash);
      //kmer_hash = MurmurHash2(kmer);
      //kmer_RC_hash = MurmurHash2(kmer_RC);
      if(cqf.count_key_value_set_traveled(kmer_hash%cqf.qf->metadata->range, kmer_count)){
        if(startKmer2unitig.find_mt(kmer, idx)){
          nodes_before_num++; node_before_x = x;
        }else if(kmer_count >= abundance_min){
          kmer_abundance_befores[x] = kmer_count;
          candidates_before[x] = true;
          candidates_before_num++;
        }
      }else if(kmer_count >= abundance_min){
        kmer_abundance_befores[x] = kmer_count;
        candidates_before[x] = true;
        candidates_before_num++;
      }
    }

    if((nodes_before_num + candidates_before_num) || (nodes_after_num+candidates_after_num)>1){ //no-linear extension
      // if(startKmer2unitig.find_mt(contig_seq.substr(0, params.K), idx)){
      //   //A contig has been constructed by another program from RC way
      //   if(abs(idx) != contig_id){
      //     contigs.set_mt(contig_id, ""); 
      //     break;
      //   }
      // }else{
      //   throw logic_error("Unexpectedly not found start kmer!");
      // }
      startKmer2unitig.insert_mt(current_kmer_RC, -contig_id);
      contigs.set_mt(contig_id, Contig(contig_seq, median(abundances)));

      for(int x= 0; x < 4; x++){
        if(candidates_after[x]){
          kmer = current_kmer_fix+DNA_bases[x];
          if(!startKmer2unitig.find_mt(kmer, idx)){
            int new_unitig_idx = contigs.push_back_mt(Contig(kmer, kmer_abundance_afters[x]));
            startKmer2unitig.insert_mt(kmer, new_unitig_idx);
            work_queue->add_work(1); 
          }
        }
      }
      kmer = current_kmer_RC;
      for(int x = 0; x < 4; x++){
        if(candidates_before[x]){
          kmer[K-1] = DNA_bases[x];
          if(!startKmer2unitig.find_mt(kmer, idx)){
            int new_unitig_idx = contigs.push_back_mt(Contig(kmer, kmer_abundance_befores[x]));
            //startKmer2unitig[kmer_befores_RC[x]] = -new_unitig_idx;
            startKmer2unitig.insert_mt(kmer, new_unitig_idx);
            work_queue->add_work(1);
          }
        }
      }
      break;
    }else if(candidates_after_num==1){ //only one candidate k-mer after
      for(int x = 0; x<4; x++){
        if(candidates_after[x]){
          current_kmer = current_kmer_fix+DNA_bases[x];
          current_kmer_RC = RC_DNAbase(DNA_bases[x])+current_kmer_RC.substr(0, K-1);
          contig_seq += DNA_bases[x];
          abundances.push_back(kmer_abundance_afters[x]);
          NTPC64('T', 'A', options.K, current_kmer_RC_hash, current_kmer_hash);
          NTPC64('T', DNA_bases[x], options.K, current_kmer_hash, current_kmer_RC_hash);
          break;
        }
      }
      continue;
    }else if (nodes_after_num==1){
      current_kmer = current_kmer_fix + DNA_bases[node_after_x];
      current_kmer_RC = RC_DNAbase(DNA_bases[node_after_x]) + current_kmer_RC.substr(0, K-1);
      contig_seq += DNA_bases[node_after_x];
      abundances.push_back(kmer_abundance_afters[node_after_x]);
      NTPC64('T', 'A', options.K, current_kmer_RC_hash, current_kmer_hash);
      NTPC64('T', DNA_bases[node_after_x], options.K, current_kmer_hash, current_kmer_RC_hash);
    }else{ //stop
      startKmer2unitig.insert_mt(current_kmer_RC, -contig_id);
      contigs.set_mt(contig_id, Contig(contig_seq, median(abundances)));
      break;
    }
  }
}
void get_unitig_forward(CQF_mt& cqf, const Params& options, vector_mt<Contig>& contigs, unordered_set_mt& startKmers, WorkQueue* work_queue, const int& contig_id){
  array<bool, 4> candidates_before({false, false, false, false}), candidates_after({false, false, false, false});
  //array<string, 4> kmer_befores, kmer_afters, kmer_befores_RC, kmer_afters_RC;
  array<uint64_t, 4> kmer_abundance_befores, kmer_abundance_afters;
  auto abundance_min = options.kmer_abundance_min;
  auto K = options.K;

  int candidates_before_num, candidates_after_num;
  int nodes_before_num, nodes_after_num;
  uint64_t kmer_hash, kmer_RC_hash, current_kmer_hash, current_kmer_RC_hash;
  string kmer, kmer_RC, current_kmer, current_kmer_RC, current_kmer_fix;
  uint64_t kmer_count, kmer_RC_count;
  int idx;
  
  string contig_seq = contigs.at_mt(contig_id).seq;
  current_kmer = contig_seq.substr(contig_seq.length()-K);
  current_kmer_RC = RC_DNA(current_kmer);

  std::vector<int> abundances;
  abundances.push_back(int(contigs.at_mt(contig_id).median_abundance));

  NTPC64(current_kmer.c_str(), K, current_kmer_hash, current_kmer_RC_hash);
  int node_after_x, node_before_x;//useful only when there is only one node after and without candidates after.
  while(true){
    //NTPC64(current_kmer.c_str(), K, current_kmer_hash, current_kmer_RC_hash);
    current_kmer_fix = current_kmer.substr(1);
    //kmer_afters = kmers_after(current_kmer);
    //kmer_befores = kmers_before(kmer_afters[0]);
    candidates_before = candidates_after = {{false, false, false, false}};
    candidates_before_num = candidates_after_num = 0;
    nodes_before_num = nodes_after_num = 0;
    
    //kmers with current_kmer_fix as prefix
    for(int x = 0; x<4; x++){
      //kmer = kmer_afters[x];
      //kmer_RC = RC_DNA(kmer);
      //kmer_afters_RC[x] = kmer_RC;
      kmer_hash = current_kmer_hash; kmer_RC_hash = current_kmer_RC_hash;
      kmer_hash = NTPC64(current_kmer[0], DNA_bases[x], K, kmer_hash, kmer_RC_hash);
      //kmer_hash = MurmurHash2(kmer);
      //kmer_RC_hash = MurmurHash2(kmer_RC);
      if(cqf.count_key_value_set_traveled(kmer_hash%cqf.qf->metadata->range, kmer_count)){
        if(startKmers.find(current_kmer_fix+DNA_bases[x]) != startKmers.end()){
          nodes_after_num++;
          kmer_abundance_afters[x] = kmer_count;
          node_after_x = x;
        }else if(kmer_count >= abundance_min){
          kmer_abundance_afters[x] = kmer_count;
          candidates_after[x] = true; //possible because of hash collisions
          candidates_after_num ++;
        }
      }else if(kmer_count >= abundance_min){
        kmer_abundance_afters[x] = kmer_count;
        candidates_after[x] = true;
        candidates_after_num ++;
      }
    }
    
    //kmers with RC(current_kmer_fix) as prefix
    NTPC64(current_kmer[0], 'A', options.K, current_kmer_hash, current_kmer_RC_hash);
    kmer = current_kmer_RC;
    for(int x = 0; x<4; x++){
      if(DNA_bases[x] == current_kmer_RC[K-1]){
        continue;
      }
      //kmer = kmer_befores[x];
      //kmer_RC = RC_DNA(kmer);
      //kmer_befores_RC[x] = kmer_RC;
      //kmer_hash = current_kmer_hash; kmer_RC_hash = current_kmer_RC_hash;
      //kmer_hash = NTPC64(current_kmer_RC[0], DNA_bases[x], K, kmer_hash, kmer_RC_hash);
      //kmer = current_kmer_RC;
      kmer[K-1] = DNA_bases[x];
      kmer_hash = current_kmer_hash; kmer_RC_hash = current_kmer_RC_hash;
      kmer_hash = NTPC64('T', DNA_bases[x], options.K, kmer_RC_hash, kmer_hash);
      //kmer_hash = NTPC64(kmer.c_str(), K, kmer_hash, kmer_RC_hash);
      //kmer_hash = MurmurHash2(kmer);
      //kmer_RC_hash = MurmurHash2(kmer_RC);
      if(cqf.count_key_value_set_traveled(kmer_hash%cqf.qf->metadata->range, kmer_count)){
        if(startKmers.find(kmer) != startKmers.end()){
          nodes_before_num++; node_before_x = x;
        }else if(kmer_count >= abundance_min){
          kmer_abundance_befores[x] = kmer_count;
          candidates_before[x] = true;
          candidates_before_num++;
        }
      }else if(kmer_count >= abundance_min){
        kmer_abundance_befores[x] = kmer_count;
        candidates_before[x] = true;
        candidates_before_num++;
      }
    }

    if((nodes_before_num + candidates_before_num) || (nodes_after_num+candidates_after_num)>1){ //no-linear extension
      // if(startKmer2unitig.find_mt(contig_seq.substr(0, params.K), idx)){
      //   //A contig has been constructed by another program from RC way
      //   if(abs(idx) != contig_id){
      //     contigs.set_mt(contig_id, ""); 
      //     break;
      //   }
      // }else{
      //   throw logic_error("Unexpectedly not found start kmer!");
      // }
      startKmers.insert(current_kmer_RC);
      contigs.set_mt(contig_id, Contig(contig_seq, median(abundances)));

      for(int x= 0; x < 4; x++){
        if(candidates_after[x]){
          kmer = current_kmer_fix+DNA_bases[x];
          if(startKmers.find(kmer)==startKmers.end()){
            int new_unitig_idx = contigs.push_back_mt(Contig(kmer, kmer_abundance_afters[x]));
            startKmers.insert(kmer);
            work_queue->add_work(1); 
          }
        }
      }
      kmer = current_kmer_RC;
      for(int x = 0; x < 4; x++){
        if(candidates_before[x]){
          kmer[K-1] = DNA_bases[x];
          if(startKmers.find(kmer) == startKmers.end()){
            int new_unitig_idx = contigs.push_back_mt(Contig(kmer, kmer_abundance_befores[x]));
            //startKmer2unitig[kmer_befores_RC[x]] = -new_unitig_idx;
            startKmers.insert(kmer);
            work_queue->add_work(1);
          }
        }
      }
      break;
    }else if(candidates_after_num==1){ //only one candidate k-mer after
      for(int x = 0; x<4; x++){
        if(candidates_after[x]){
          current_kmer = current_kmer_fix+DNA_bases[x];
          current_kmer_RC = RC_DNAbase(DNA_bases[x])+current_kmer_RC.substr(0, K-1);
          contig_seq += DNA_bases[x];
          abundances.push_back(kmer_abundance_afters[x]);
          NTPC64('T', 'A', options.K, current_kmer_RC_hash, current_kmer_hash);
          NTPC64('T', DNA_bases[x], options.K, current_kmer_hash, current_kmer_RC_hash);
          break;
        }
      }
      continue;
    }else if (nodes_after_num==1){//be careful about circles
      current_kmer = current_kmer_fix + DNA_bases[node_after_x];
      current_kmer_RC = RC_DNAbase(DNA_bases[node_after_x]) + current_kmer_RC.substr(0, K-1);
      contig_seq += DNA_bases[node_after_x];
      abundances.push_back(kmer_abundance_afters[node_after_x]);
      NTPC64('T', 'A', options.K, current_kmer_RC_hash, current_kmer_hash);
      NTPC64('T', DNA_bases[node_after_x], options.K, current_kmer_hash, current_kmer_RC_hash);
    }else{ //stop
      startKmers.insert(current_kmer_RC);
      contigs.set_mt(contig_id, Contig(contig_seq, median(abundances)));
      break;
    }
  }
}

void get_unitig_forward(CQF_mt& cqf, const Params& options, vector_mt<Contig>& contigs, unordered_map_mt<string, int>& startKmer2unitig, WorkQueue* work_queue, Contig& contig){
  int contig_id = 0;//fake contig id

  array<bool, 4> candidates_before({false, false, false, false}), candidates_after({false, false, false, false});
  //array<string, 4> kmer_befores, kmer_afters, kmer_befores_RC, kmer_afters_RC;
  array<uint64_t, 4> kmer_abundance_befores, kmer_abundance_afters;
  auto abundance_min = options.kmer_abundance_min;
  auto K = options.K;

  int candidates_before_num, candidates_after_num;
  int nodes_before_num, nodes_after_num;
  uint64_t kmer_hash, kmer_RC_hash, current_kmer_hash, current_kmer_RC_hash;
  string kmer, kmer_RC, current_kmer, current_kmer_RC, current_kmer_fix;
  uint64_t kmer_count, kmer_RC_count;
  int idx;
  
  string contig_seq = contig.seq;
  current_kmer = contig_seq.substr(contig_seq.length()-K);
  current_kmer_RC = RC_DNA(current_kmer);

  std::vector<int> abundances;
  abundances.push_back(int(contig.median_abundance));

  NTPC64(current_kmer.c_str(), K, current_kmer_hash, current_kmer_RC_hash);
  int node_after_x, node_before_x;//useful only when there is only one node after and without candidates after.
  while(true){
    //NTPC64(current_kmer.c_str(), K, current_kmer_hash, current_kmer_RC_hash);
    current_kmer_fix = current_kmer.substr(1);
    //kmer_afters = kmers_after(current_kmer);
    //kmer_befores = kmers_before(kmer_afters[0]);
    candidates_before = candidates_after = {{false, false, false, false}};
    candidates_before_num = candidates_after_num = 0;
    nodes_before_num = nodes_after_num = 0;
    
    //kmers with current_kmer_fix as prefix
    for(int x = 0; x<4; x++){
      //kmer = kmer_afters[x];
      //kmer_RC = RC_DNA(kmer);
      //kmer_afters_RC[x] = kmer_RC;
      kmer_hash = current_kmer_hash; kmer_RC_hash = current_kmer_RC_hash;
      kmer_hash = NTPC64(current_kmer[0], DNA_bases[x], K, kmer_hash, kmer_RC_hash);
      //kmer_hash = MurmurHash2(kmer);
      //kmer_RC_hash = MurmurHash2(kmer_RC);
      if(cqf.count_key_value_set_traveled(kmer_hash%cqf.qf->metadata->range, kmer_count)){
        if(startKmer2unitig.find_mt(current_kmer_fix+DNA_bases[x], idx)){
          nodes_after_num++;
          kmer_abundance_afters[x] = kmer_count;
          node_after_x = x;
        }else if(kmer_count >= abundance_min){
          kmer_abundance_afters[x] = kmer_count;
          candidates_after[x] = true; //possible because of hash collisions
          candidates_after_num ++;
        }
      }else if(kmer_count >= abundance_min){
        kmer_abundance_afters[x] = kmer_count;
        candidates_after[x] = true;
        candidates_after_num ++;
      }
    }
    
    //kmers with RC(current_kmer_fix) as prefix
    NTPC64(current_kmer[0], 'A', options.K, current_kmer_hash, current_kmer_RC_hash);
    kmer = current_kmer_RC;
    for(int x = 0; x<4; x++){
      if(DNA_bases[x] == current_kmer_RC[K-1]){
        continue;
      }
      //kmer = kmer_befores[x];
      //kmer_RC = RC_DNA(kmer);
      //kmer_befores_RC[x] = kmer_RC;
      //kmer_hash = current_kmer_hash; kmer_RC_hash = current_kmer_RC_hash;
      //kmer_hash = NTPC64(current_kmer_RC[0], DNA_bases[x], K, kmer_hash, kmer_RC_hash);
      //kmer = current_kmer_RC;
      kmer[K-1] = DNA_bases[x];
      kmer_hash = current_kmer_hash; kmer_RC_hash = current_kmer_RC_hash;
      kmer_hash = NTPC64('T', DNA_bases[x], options.K, kmer_RC_hash, kmer_hash);
      //kmer_hash = NTPC64(kmer.c_str(), K, kmer_hash, kmer_RC_hash);
      //kmer_hash = MurmurHash2(kmer);
      //kmer_RC_hash = MurmurHash2(kmer_RC);
      if(cqf.count_key_value_set_traveled(kmer_hash%cqf.qf->metadata->range, kmer_count)){
        if(startKmer2unitig.find_mt(kmer, idx)){
          nodes_before_num++; node_before_x = x;
        }else if(kmer_count >= abundance_min){
          kmer_abundance_befores[x] = kmer_count;
          candidates_before[x] = true;
          candidates_before_num++;
        }
      }else if(kmer_count >= abundance_min){
        kmer_abundance_befores[x] = kmer_count;
        candidates_before[x] = true;
        candidates_before_num++;
      }
    }

    if((nodes_before_num + candidates_before_num) || (nodes_after_num+candidates_after_num)>1){ //no-linear extension
      // if(startKmer2unitig.find_mt(contig_seq.substr(0, params.K), idx)){
      //   //A contig has been constructed by another program from RC way
      //   if(abs(idx) != contig_id){
      //     contigs.set_mt(contig_id, ""); 
      //     break;
      //   }
      // }else{
      //   throw logic_error("Unexpectedly not found start kmer!");
      // }
      startKmer2unitig.insert_mt(current_kmer_RC, -contig_id);
      //contigs.set_mt(contig_id, Contig(contig_seq, median(abundances)));
      contig.seq = contig_seq;
      contig.median_abundance = median(abundances);

      for(int x= 0; x < 4; x++){
        if(candidates_after[x]){
          kmer = current_kmer_fix+DNA_bases[x];
          if(!startKmer2unitig.find_mt(kmer, idx)){
            int new_unitig_idx = contigs.push_back_mt(Contig(kmer, kmer_abundance_afters[x]));
            startKmer2unitig.insert_mt(kmer, new_unitig_idx);
            work_queue->add_work(1); 
          }
        }
      }
      kmer = current_kmer_RC;
      for(int x = 0; x < 4; x++){
        if(candidates_before[x]){
          kmer[K-1] = DNA_bases[x];
          if(!startKmer2unitig.find_mt(kmer, idx)){
            int new_unitig_idx = contigs.push_back_mt(Contig(kmer, kmer_abundance_befores[x]));
            //startKmer2unitig[kmer_befores_RC[x]] = -new_unitig_idx;
            startKmer2unitig.insert_mt(kmer, new_unitig_idx);
            work_queue->add_work(1);
          }
        }
      }
      break;
    }else if(candidates_after_num==1){ //only one candidate k-mer after
      for(int x = 0; x<4; x++){
        if(candidates_after[x]){
          current_kmer = current_kmer_fix+DNA_bases[x];
          current_kmer_RC = RC_DNAbase(DNA_bases[x])+current_kmer_RC.substr(0, K-1);
          contig_seq += DNA_bases[x];
          abundances.push_back(kmer_abundance_afters[x]);
          NTPC64('T', 'A', options.K, current_kmer_RC_hash, current_kmer_hash);
          NTPC64('T', DNA_bases[x], options.K, current_kmer_hash, current_kmer_RC_hash);
          break;
        }
      }
      continue;
    }else if (nodes_after_num==1){
      current_kmer = current_kmer_fix + DNA_bases[node_after_x];
      current_kmer_RC = RC_DNAbase(DNA_bases[node_after_x]) + current_kmer_RC.substr(0, K-1);
      contig_seq += DNA_bases[node_after_x];
      abundances.push_back(kmer_abundance_afters[node_after_x]);
      NTPC64('T', 'A', options.K, current_kmer_RC_hash, current_kmer_hash);
      NTPC64('T', DNA_bases[node_after_x], options.K, current_kmer_hash, current_kmer_RC_hash);
    }else{ //stop
      startKmer2unitig.insert_mt(current_kmer_RC, -contig_id);
      //contigs.set_mt(contig_id, Contig(contig_seq, median(abundances)));
      contig.seq = contig_seq;
      contig.median_abundance = median(abundances);
      break;
    }
  }
}
*/
void get_unitig_forward(CQF_mt& cqf, const Params& options, concurrent_vector<Contig>& contigs, unordered_set_mt& startKmers, WorkQueue* work_queue, concurrent_vector<Contig>::iterator& contigIter){
  array<bool, 4> candidates_before({false, false, false, false}), candidates_after({false, false, false, false});
  //array<string, 4> kmer_befores, kmer_afters, kmer_befores_RC, kmer_afters_RC;
  array<uint64_t, 4> kmer_abundance_befores, kmer_abundance_afters;
  auto abundance_min = options.kmer_abundance_min;
  auto K = options.K;

  int candidates_before_num, candidates_after_num;
  int nodes_before_num, nodes_after_num;
  uint64_t kmer_hash, kmer_RC_hash, current_kmer_hash, current_kmer_RC_hash;
  DNAString kmer, kmer_RC, current_kmer, current_kmer_RC, current_kmer_fix;
  uint64_t kmer_count, kmer_RC_count;
  int idx;
  
  DNAString& contig_seq = contigIter->seq;
  //string contig_seq = contigIter->seq.to_str();
  current_kmer = contig_seq.substr(contig_seq.length()-K);
  current_kmer_RC = current_kmer.get_RC();

  std::vector<int> abundances;
  abundances.push_back(int(contigIter->median_abundance));

  //NTPC64(current_kmer.c_str(), K, current_kmer_hash, current_kmer_RC_hash);
  NTPC64(current_kmer, K, current_kmer_hash, current_kmer_RC_hash);
  int node_after_x, node_before_x;//useful only when there is only one node after and without candidates after.
  while(true){
    //NTPC64(current_kmer.c_str(), K, current_kmer_hash, current_kmer_RC_hash);
    current_kmer_fix = current_kmer.substr(1);
    //kmer_afters = kmers_after(current_kmer);
    //kmer_befores = kmers_before(kmer_afters[0]);
    candidates_before = candidates_after = {{false, false, false, false}};
    candidates_before_num = candidates_after_num = 0;
    nodes_before_num = nodes_after_num = 0;
    
    //kmers with current_kmer_fix as prefix
    for(int x = 0; x<4; x++){
      //kmer = kmer_afters[x];
      //kmer_RC = RC_DNA(kmer);
      //kmer_afters_RC[x] = kmer_RC;
      kmer_hash = current_kmer_hash; kmer_RC_hash = current_kmer_RC_hash;
      kmer_hash = NTPC64(current_kmer[0], DNA_bases[x], K, kmer_hash, kmer_RC_hash);
      //kmer_hash = MurmurHash2(kmer);
      //kmer_RC_hash = MurmurHash2(kmer_RC);
      bool isTraveled=cqf.count_key_value_set_traveled(kmer_hash%cqf.qf->metadata->range, kmer_count);
      if(kmer_count>= abundance_min){//if not traveled, then go
        if(isTraveled && startKmers.find(current_kmer_fix+DNA_bases[x]) != startKmers.end()){
          nodes_after_num++;
          kmer_abundance_afters[x] = kmer_count;
          node_after_x = x;
        //}else if(kmer_count >= abundance_min){
        }else{
          kmer_abundance_afters[x] = kmer_count;
          candidates_after[x] = true; //possible because of hash collisions
          candidates_after_num ++;
        }
      }
    }
    
    //kmers with RC(current_kmer_fix) as prefix
    NTPC64(current_kmer[0], 'A', options.K, current_kmer_hash, current_kmer_RC_hash);
    kmer = current_kmer_RC;
    for(int x = 0; x<4; x++){
      if(DNA_bases[x] == current_kmer_RC[K-1]){
        continue;
      }
      //kmer = kmer_befores[x];
      //kmer_RC = RC_DNA(kmer);
      //kmer_befores_RC[x] = kmer_RC;
      //kmer_hash = current_kmer_hash; kmer_RC_hash = current_kmer_RC_hash;
      //kmer_hash = NTPC64(current_kmer_RC[0], DNA_bases[x], K, kmer_hash, kmer_RC_hash);
      //kmer = current_kmer_RC;
      kmer.replace(K-1, DNA_bases[x]);
      kmer_hash = current_kmer_hash; kmer_RC_hash = current_kmer_RC_hash;
      kmer_hash = NTPC64('T', DNA_bases[x], options.K, kmer_RC_hash, kmer_hash);
      //kmer_hash = NTPC64(kmer.c_str(), K, kmer_hash, kmer_RC_hash);
      //kmer_hash = MurmurHash2(kmer);
      //kmer_RC_hash = MurmurHash2(kmer_RC);
      bool isTraveled = cqf.count_key_value_set_traveled(kmer_hash%cqf.qf->metadata->range, kmer_count);
      if(kmer_count >= abundance_min){
        if(isTraveled && startKmers.find(kmer) != startKmers.end()){
          nodes_before_num++; node_before_x = x;
        //}else if(kmer_count >= abundance_min){
        }else{
          kmer_abundance_befores[x] = kmer_count;
          candidates_before[x] = true;
          candidates_before_num++;
        }
      }
    }

    if((nodes_before_num + candidates_before_num) || (nodes_after_num+candidates_after_num)>1){ //no-linear extension
      // if(startKmer2unitig.find_mt(contig_seq.substr(0, params.K), idx)){
      //   //A contig has been constructed by another program from RC way
      //   if(abs(idx) != contig_id){
      //     contigs.set_mt(contig_id, ""); 
      //     break;
      //   }
      // }else{
      //   throw logic_error("Unexpectedly not found start kmer!");
      // }
      startKmers.insert(current_kmer_RC);
      //contigs.set_mt(contig_id, Contig(contig_seq, median(abundances)));
      //contigIter->seq = contig_seq;
      contigIter->median_abundance = median(abundances);

      for(int x= 0; x < 4; x++){
        if(candidates_after[x]){
          kmer = current_kmer_fix+DNA_bases[x];
          if(startKmers.find(kmer)==startKmers.end()){
            startKmers.insert(kmer);
            auto it = contigs.push_back(Contig(kmer, kmer_abundance_afters[x]));
            work_queue->add_work(it); 
          }
        }
      }
      kmer = current_kmer_RC;
      for(int x = 0; x < 4; x++){
        if(candidates_before[x]){
          kmer.replace(K-1, DNA_bases[x]);
          if(startKmers.find(kmer) == startKmers.end()){
            startKmers.insert(kmer);
            auto it = contigs.push_back(Contig(kmer, kmer_abundance_befores[x]));
            //startKmer2unitig[kmer_befores_RC[x]] = -new_unitig_idx;
            work_queue->add_work(it);
          }
        }
      }
      break;
    }else if(candidates_after_num==1){ //only one candidate k-mer after
      for(int x = 0; x<4; x++){
        if(candidates_after[x]){
          current_kmer = current_kmer_fix+DNA_bases[x];
          current_kmer_RC = RC_DNAbase(DNA_bases[x])+current_kmer_RC.substr(0, K-1);
          contig_seq += DNA_bases[x];
          abundances.push_back(kmer_abundance_afters[x]);
          NTPC64('T', 'A', options.K, current_kmer_RC_hash, current_kmer_hash);
          NTPC64('T', DNA_bases[x], options.K, current_kmer_hash, current_kmer_RC_hash);
          break;
        }
      }
      continue;
    }else if (nodes_after_num==1){//be careful about circles
      current_kmer = current_kmer_fix + DNA_bases[node_after_x];
      current_kmer_RC = RC_DNAbase(DNA_bases[node_after_x]) + current_kmer_RC.substr(0, K-1);
      contig_seq += DNA_bases[node_after_x];
      abundances.push_back(kmer_abundance_afters[node_after_x]);
      NTPC64('T', 'A', options.K, current_kmer_RC_hash, current_kmer_hash);
      NTPC64('T', DNA_bases[node_after_x], options.K, current_kmer_hash, current_kmer_RC_hash);
    }else{ //stop
      startKmers.insert(current_kmer_RC);
      //contigIter->seq = contig_seq;
      contigIter->median_abundance = median(abundances);
      //contigs.set_mt(contig_id, Contig(contig_seq, median(abundances)));
      break;
    }
  }
}

//True if it was inserted. 
//if dnaStr exists, insert only when idx is smaller than existing one; otherwise simply insert the new record. 
bool insert_or_replace(hash_map_mt& startKmer2unitig, const DNAString& dnastr, const size_t idx){
  hash_map_mt::accessor access;
  if(startKmer2unitig.insert(access, dnastr) || access->second >= idx){//True if new pair was inserted; false if key was already in the map.
    access->second = idx;
    return true;
  }
  return false;
}

//TODO
void get_unitig_forward(CQF_mt& cqf, const Params& options, concurrent_vector<Contig>& contigs, hash_map_mt& startKmer2unitig, WorkQueue* work_queue, concurrent_vector<Contig>::iterator& contigIter){
  array<bool, 4> candidates_before({false, false, false, false}), candidates_after({false, false, false, false});
  //array<string, 4> kmer_befores, kmer_afters, kmer_befores_RC, kmer_afters_RC;
  array<uint64_t, 4> kmer_abundance_befores, kmer_abundance_afters;
  auto abundance_min = options.kmer_abundance_min;
  auto K = options.K;

  int candidates_before_num, candidates_after_num;
  int nodes_before_num, nodes_after_num;
  uint64_t kmer_hash, kmer_RC_hash, current_kmer_hash, current_kmer_RC_hash;
  DNAString kmer, kmer_RC, current_kmer, current_kmer_RC, current_kmer_fix;
  uint64_t kmer_count, kmer_RC_count;
  int idx;
  
  DNAString& contig_seq = contigIter->seq;
  //string contig_seq = contigIter->seq.to_str();
  current_kmer = contig_seq.substr(contig_seq.length()-K);
  current_kmer_RC = current_kmer.get_RC();

  std::vector<int> abundances(contigIter->seq.length()-K+1, int(contigIter->median_abundance));

  hash_map_mt::const_accessor const_access;
  //NTPC64(current_kmer.c_str(), K, current_kmer_hash, current_kmer_RC_hash);
  NTPC64(current_kmer, K, current_kmer_hash, current_kmer_RC_hash);
  int node_after_x, node_before_x;//useful only when there is only one node after and without candidates after.
  while(true){
    //NTPC64(current_kmer.c_str(), K, current_kmer_hash, current_kmer_RC_hash);
    current_kmer_fix = current_kmer.substr(1);
    //kmer_afters = kmers_after(current_kmer);
    //kmer_befores = kmers_before(kmer_afters[0]);
    candidates_before = candidates_after = {{false, false, false, false}};
    candidates_before_num = candidates_after_num = 0;
    nodes_before_num = nodes_after_num = 0;
    
    //kmers with current_kmer_fix as prefix
    for(int x = 0; x<4; x++){
      //kmer = kmer_afters[x];
      //kmer_RC = RC_DNA(kmer);
      //kmer_afters_RC[x] = kmer_RC;
      kmer_hash = current_kmer_hash; kmer_RC_hash = current_kmer_RC_hash;
      kmer_hash = NTPC64(current_kmer[0], DNA_bases[x], K, kmer_hash, kmer_RC_hash);
      //kmer_hash = MurmurHash2(kmer);
      //kmer_RC_hash = MurmurHash2(kmer_RC);

      bool isTraveled=cqf.count_key_value_set_traveled(kmer_hash%cqf.qf->metadata->range, kmer_count);
      if(kmer_count>= abundance_min){//if not traveled, then go
        if(isTraveled && startKmer2unitig.find(const_access, current_kmer_fix+DNA_bases[x])){
          nodes_after_num++;
          kmer_abundance_afters[x] = kmer_count;
          node_after_x = x;
        //}else if(kmer_count >= abundance_min){
        }else{
          kmer_abundance_afters[x] = kmer_count;
          candidates_after[x] = true; //possible because of hash collisions
          candidates_after_num ++;
        }
        const_access.release();
      }
    }
    
    //kmers with RC(current_kmer_fix) as prefix
    NTPC64(current_kmer[0], 'A', options.K, current_kmer_hash, current_kmer_RC_hash);
    kmer = current_kmer_RC;
    for(int x = 0; x<4; x++){
      if(DNA_bases[x] == current_kmer_RC[K-1]){
        continue;
      }
      //kmer = kmer_befores[x];
      //kmer_RC = RC_DNA(kmer);
      //kmer_befores_RC[x] = kmer_RC;
      //kmer_hash = current_kmer_hash; kmer_RC_hash = current_kmer_RC_hash;
      //kmer_hash = NTPC64(current_kmer_RC[0], DNA_bases[x], K, kmer_hash, kmer_RC_hash);
      //kmer = current_kmer_RC;
      kmer.replace(K-1, DNA_bases[x]);
      kmer_hash = current_kmer_hash; kmer_RC_hash = current_kmer_RC_hash;
      kmer_hash = NTPC64('T', DNA_bases[x], options.K, kmer_RC_hash, kmer_hash);
      //kmer_hash = NTPC64(kmer.c_str(), K, kmer_hash, kmer_RC_hash);
      //kmer_hash = MurmurHash2(kmer);
      //kmer_RC_hash = MurmurHash2(kmer_RC);
      bool isTraveled = cqf.count_key_value_set_traveled(kmer_hash%cqf.qf->metadata->range, kmer_count);
      if(kmer_count >= abundance_min){
        if(isTraveled && startKmer2unitig.find(const_access, kmer)){
          nodes_before_num++; node_before_x = x;
        //}else if(kmer_count >= abundance_min){
        }else{
          kmer_abundance_befores[x] = kmer_count;
          candidates_before[x] = true;
          candidates_before_num++;
        }
        const_access.release();
      }
    }

    if((nodes_before_num + candidates_before_num) || (nodes_after_num+candidates_after_num)>1){ //no-linear extension
      // if(startKmer2unitig.find_mt(contig_seq.substr(0, params.K), idx)){
      //   //A contig has been constructed by another program from RC way
      //   if(abs(idx) != contig_id){
      //     contigs.set_mt(contig_id, ""); 
      //     break;
      //   }
      // }else{
      //   throw logic_error("Unexpectedly not found start kmer!");
      // }
      if(!insert_or_replace(startKmer2unitig, DNAString(current_kmer_RC), (contigIter-contigs.begin()))){
        contigIter->clear();
        break;
      }

      //startKmers.insert(current_kmer_RC);
      contigIter->median_abundance = median(abundances);

      for(int x= 0; x < 4; x++){
        if(candidates_after[x]){
          kmer = current_kmer_fix+DNA_bases[x];

          hash_map_mt::accessor access;
          if(startKmer2unitig.insert(access, kmer)){
            auto it = contigs.push_back(Contig(kmer, kmer_abundance_afters[x]));
            access->second = it - contigs.begin();
            work_queue->add_work(it);
          }
          access.release();
        }
      }
      kmer = current_kmer_RC;
      for(int x = 0; x < 4; x++){
        if(candidates_before[x]){
          kmer.replace(K-1, DNA_bases[x]);
          hash_map_mt::accessor access;
          if(startKmer2unitig.insert(access, kmer)){
            auto it = contigs.push_back(Contig(kmer, kmer_abundance_befores[x]));
            access->second = it - contigs.begin();
            work_queue->add_work(it);
          }
          access.release();
        }
      }
      break;
    }else if(candidates_after_num==1){ //only one candidate k-mer after
      for(int x = 0; x<4; x++){
        if(candidates_after[x]){
          current_kmer = current_kmer_fix+DNA_bases[x];
          current_kmer_RC = RC_DNAbase(DNA_bases[x])+current_kmer_RC.substr(0, K-1);
          contig_seq += DNA_bases[x];
          abundances.push_back(kmer_abundance_afters[x]);
          NTPC64('T', 'A', options.K, current_kmer_RC_hash, current_kmer_hash);
          NTPC64('T', DNA_bases[x], options.K, current_kmer_hash, current_kmer_RC_hash);
          break;
        }
      }
      continue;
    }else if (nodes_after_num==1){//be careful about circles
      current_kmer = current_kmer_fix + DNA_bases[node_after_x];
      current_kmer_RC = RC_DNAbase(DNA_bases[node_after_x]) + current_kmer_RC.substr(0, K-1);
      contig_seq += DNA_bases[node_after_x];
      abundances.push_back(kmer_abundance_afters[node_after_x]);
      NTPC64('T', 'A', options.K, current_kmer_RC_hash, current_kmer_hash);
      NTPC64('T', DNA_bases[node_after_x], options.K, current_kmer_hash, current_kmer_RC_hash);
    }else{ //stop
      if(!insert_or_replace(startKmer2unitig, DNAString(current_kmer_RC), (contigIter-contigs.begin()))){
        contigIter->clear();
      }else{
        contigIter->median_abundance = median(abundances);
      }
      //startKmers.insert(current_kmer_RC);
      //contigIter->seq = contig_seq;
      
      //contigs.set_mt(contig_id, Contig(contig_seq, median(abundances)));
      break;
    }
  }
}

/*
void get_unitig_forward(CQF_mt& cqf, const Params& options, concurrent_vector<Contig>& contigs, unordered_map_mt<string, int>& startKmer2unitig, WorkQueue* work_queue, Contig& contig){
  int contig_id = 0;//fake contig id

  array<bool, 4> candidates_before({false, false, false, false}), candidates_after({false, false, false, false});
  //array<string, 4> kmer_befores, kmer_afters, kmer_befores_RC, kmer_afters_RC;
  array<uint64_t, 4> kmer_abundance_befores, kmer_abundance_afters;
  auto abundance_min = options.kmer_abundance_min;
  auto K = options.K;

  int candidates_before_num, candidates_after_num;
  int nodes_before_num, nodes_after_num;
  uint64_t kmer_hash, kmer_RC_hash, current_kmer_hash, current_kmer_RC_hash;
  string kmer, kmer_RC, current_kmer, current_kmer_RC, current_kmer_fix;
  uint64_t kmer_count, kmer_RC_count;
  int idx;
  
  string contig_seq = contig.seq;
  current_kmer = contig_seq.substr(contig_seq.length()-K);
  current_kmer_RC = RC_DNA(current_kmer);

  std::vector<int> abundances;
  abundances.push_back(int(contig.median_abundance));

  NTPC64(current_kmer.c_str(), K, current_kmer_hash, current_kmer_RC_hash);
  int node_after_x, node_before_x;//useful only when there is only one node after and without candidates after.
  while(true){
    //NTPC64(current_kmer.c_str(), K, current_kmer_hash, current_kmer_RC_hash);
    current_kmer_fix = current_kmer.substr(1);
    //kmer_afters = kmers_after(current_kmer);
    //kmer_befores = kmers_before(kmer_afters[0]);
    candidates_before = candidates_after = {{false, false, false, false}};
    candidates_before_num = candidates_after_num = 0;
    nodes_before_num = nodes_after_num = 0;
    
    //kmers with current_kmer_fix as prefix
    for(int x = 0; x<4; x++){
      //kmer = kmer_afters[x];
      //kmer_RC = RC_DNA(kmer);
      //kmer_afters_RC[x] = kmer_RC;
      kmer_hash = current_kmer_hash; kmer_RC_hash = current_kmer_RC_hash;
      kmer_hash = NTPC64(current_kmer[0], DNA_bases[x], K, kmer_hash, kmer_RC_hash);
      //kmer_hash = MurmurHash2(kmer);
      //kmer_RC_hash = MurmurHash2(kmer_RC);
      if(cqf.count_key_value_set_traveled(kmer_hash%cqf.qf->metadata->range, kmer_count)){
        if(startKmer2unitig.find_mt(current_kmer_fix+DNA_bases[x], idx)){
          nodes_after_num++;
          kmer_abundance_afters[x] = kmer_count;
          node_after_x = x;
        }else if(kmer_count >= abundance_min){
          kmer_abundance_afters[x] = kmer_count;
          candidates_after[x] = true; //possible because of hash collisions
          candidates_after_num ++;
        }
      }else if(kmer_count >= abundance_min){
        kmer_abundance_afters[x] = kmer_count;
        candidates_after[x] = true;
        candidates_after_num ++;
      }
    }
    
    //kmers with RC(current_kmer_fix) as prefix
    NTPC64(current_kmer[0], 'A', options.K, current_kmer_hash, current_kmer_RC_hash);
    kmer = current_kmer_RC;
    for(int x = 0; x<4; x++){
      if(DNA_bases[x] == current_kmer_RC[K-1]){
        continue;
      }
      //kmer = kmer_befores[x];
      //kmer_RC = RC_DNA(kmer);
      //kmer_befores_RC[x] = kmer_RC;
      //kmer_hash = current_kmer_hash; kmer_RC_hash = current_kmer_RC_hash;
      //kmer_hash = NTPC64(current_kmer_RC[0], DNA_bases[x], K, kmer_hash, kmer_RC_hash);
      //kmer = current_kmer_RC;
      kmer[K-1] = DNA_bases[x];
      kmer_hash = current_kmer_hash; kmer_RC_hash = current_kmer_RC_hash;
      kmer_hash = NTPC64('T', DNA_bases[x], options.K, kmer_RC_hash, kmer_hash);
      //kmer_hash = NTPC64(kmer.c_str(), K, kmer_hash, kmer_RC_hash);
      //kmer_hash = MurmurHash2(kmer);
      //kmer_RC_hash = MurmurHash2(kmer_RC);
      if(cqf.count_key_value_set_traveled(kmer_hash%cqf.qf->metadata->range, kmer_count)){
        if(startKmer2unitig.find_mt(kmer, idx)){
          nodes_before_num++; node_before_x = x;
        }else if(kmer_count >= abundance_min){
          kmer_abundance_befores[x] = kmer_count;
          candidates_before[x] = true;
          candidates_before_num++;
        }
      }else if(kmer_count >= abundance_min){
        kmer_abundance_befores[x] = kmer_count;
        candidates_before[x] = true;
        candidates_before_num++;
      }
    }

    if((nodes_before_num + candidates_before_num) || (nodes_after_num+candidates_after_num)>1){ //no-linear extension
      // if(startKmer2unitig.find_mt(contig_seq.substr(0, params.K), idx)){
      //   //A contig has been constructed by another program from RC way
      //   if(abs(idx) != contig_id){
      //     contigs.set_mt(contig_id, ""); 
      //     break;
      //   }
      // }else{
      //   throw logic_error("Unexpectedly not found start kmer!");
      // }
      startKmer2unitig.insert_mt(current_kmer_RC, -contig_id);
      //contigs.set_mt(contig_id, Contig(contig_seq, median(abundances)));
      contig.seq = contig_seq;
      contig.median_abundance = median(abundances);

      for(int x= 0; x < 4; x++){
        if(candidates_after[x]){
          kmer = current_kmer_fix+DNA_bases[x];
          if(!startKmer2unitig.find_mt(kmer, idx)){
            int new_unitig_idx = contigs.push_back_mt(Contig(kmer, kmer_abundance_afters[x]));
            startKmer2unitig.insert_mt(kmer, new_unitig_idx);
            work_queue->add_work(1); 
          }
        }
      }
      kmer = current_kmer_RC;
      for(int x = 0; x < 4; x++){
        if(candidates_before[x]){
          kmer[K-1] = DNA_bases[x];
          if(!startKmer2unitig.find_mt(kmer, idx)){
            int new_unitig_idx = contigs.push_back_mt(Contig(kmer, kmer_abundance_befores[x]));
            //startKmer2unitig[kmer_befores_RC[x]] = -new_unitig_idx;
            startKmer2unitig.insert_mt(kmer, new_unitig_idx);
            work_queue->add_work(1);
          }
        }
      }
      break;
    }else if(candidates_after_num==1){ //only one candidate k-mer after
      for(int x = 0; x<4; x++){
        if(candidates_after[x]){
          current_kmer = current_kmer_fix+DNA_bases[x];
          current_kmer_RC = RC_DNAbase(DNA_bases[x])+current_kmer_RC.substr(0, K-1);
          contig_seq += DNA_bases[x];
          abundances.push_back(kmer_abundance_afters[x]);
          NTPC64('T', 'A', options.K, current_kmer_RC_hash, current_kmer_hash);
          NTPC64('T', DNA_bases[x], options.K, current_kmer_hash, current_kmer_RC_hash);
          break;
        }
      }
      continue;
    }else if (nodes_after_num==1){
      current_kmer = current_kmer_fix + DNA_bases[node_after_x];
      current_kmer_RC = RC_DNAbase(DNA_bases[node_after_x]) + current_kmer_RC.substr(0, K-1);
      contig_seq += DNA_bases[node_after_x];
      abundances.push_back(kmer_abundance_afters[node_after_x]);
      NTPC64('T', 'A', options.K, current_kmer_RC_hash, current_kmer_hash);
      NTPC64('T', DNA_bases[node_after_x], options.K, current_kmer_hash, current_kmer_RC_hash);
    }else{ //stop
      startKmer2unitig.insert_mt(current_kmer_RC, -contig_id);
      //contigs.set_mt(contig_id, Contig(contig_seq, median(abundances)));
      contig.seq = contig_seq;
      contig.median_abundance = median(abundances);
      break;
    }
  }
}
*/

/*
void get_unitig_backward(CQF_mt& cqf, const Params& params, vector_mt<string>& contigs, unordered_map_mt<string, int>& startKmer2unitig, WorkQueue* work_queue, int contig_id){
  array<bool, 4> candidates_before, candidates_after;
  array<string, 4> kmer_befores, kmer_afters, kmer_befores_RC, kmer_afters_RC;
  int candidates_before_num, candidates_after_num;
  int nodes_before_num, nodes_after_num;
  uint64_t kmer_hash, kmer_RC_hash;
  string kmer, kmer_RC, current_kmer;
  uint64_t kmer_count, kmer_RC_count;
  int idx;

  string contig_seq = contigs.at_mt(contig_id); 
  current_kmer = contig_seq.substr(0, params.K);
  while(true){
    candidates_before = candidates_after = {{false, false, false, false}};
    kmer_befores = kmers_before(current_kmer);
    kmer_afters = kmers_after(kmer_befores[0]);
    candidates_before_num = candidates_after_num = 0;
    nodes_before_num = nodes_after_num = 0;

    for(int x = 0; x<4; x++){
      if(kmer_afters[x]==current_kmer){
        continue;
      }
      kmer = kmer_afters[x];
      kmer_RC = RC_DNA(kmer);
      kmer_afters_RC[x] = kmer_RC;

      NTPC64(kmer.c_str(), params.K, kmer_hash, kmer_RC_hash);
      //kmer_hash = MurmurHash2(kmer);
      //kmer_RC_hash = MurmurHash2(kmer_RC);
      if(cqf.count_key_value_set_traveled(kmer_hash%cqf.qf->metadata->range, kmer_count) && cqf.count_key_value_set_traveled(kmer_RC_hash%cqf.qf->metadata->range, kmer_RC_count)){
        if(startKmer2unitig.find_mt(kmer, idx)){
          nodes_after_num++;
        }else if(kmer_count >= params.CONTIG_min_cov && kmer_RC_count >= params.CONTIG_min_cov){
          candidates_after[x] = true;
          candidates_after_num ++;
        }
      }else if(kmer_count >= params.CONTIG_min_cov && kmer_RC_count >= params.CONTIG_min_cov){
        candidates_after[x] = true;
        candidates_after_num ++;        
      }
    }  
    for(int x = 0; x<4; x++){
      kmer = kmer_befores[x];
      kmer_RC = RC_DNA(kmer);
      kmer_befores_RC[x] = kmer_RC;

      NTPC64(kmer.c_str(), params.K, kmer_hash, kmer_RC_hash);
      //kmer_hash = MurmurHash2(kmer);
      //kmer_RC_hash  = MurmurHash2(kmer_RC);
      if(cqf.count_key_value_set_traveled(kmer_hash%cqf.qf->metadata->range, kmer_count) && cqf.count_key_value_set_traveled(kmer_RC_hash%cqf.qf->metadata->range, kmer_RC_count)){
        if(startKmer2unitig.find_mt(kmer_RC, idx)){
          nodes_before_num++; 
        }else if (kmer_count >= params.CONTIG_min_cov && kmer_RC_count >= params.CONTIG_min_cov){
          candidates_before[x] = true;
          candidates_before_num++;
        }
      }else if(kmer_count >= params.CONTIG_min_cov && kmer_RC_count >= params.CONTIG_min_cov){
        candidates_before[x] = true;
        candidates_before_num++;
      }
    }
    if((nodes_before_num + candidates_before_num)>1 || (nodes_after_num+candidates_after_num)){
      if(startKmer2unitig.find_mt(RC_DNA(contig_seq.substr(contig_seq.length()-params.K)), idx)){
        //A contig has been constructed by another program from RC way
        if(abs(idx) != contig_id){
          contigs.set_mt(contig_id, ""); 
          break;
        }
      }else{
        throw logic_error("Unexpectedly not found start kmer!");
      }
      startKmer2unitig.insert_mt(current_kmer, contig_id);
      
      for(int x= 0; x < 4; x++){
        if(candidates_after[x]){
          if(!startKmer2unitig.find_mt(kmer_afters[x], idx)){
            int new_unitig_idx = contigs.push_back_mt(kmer_afters[x]);
            startKmer2unitig.insert_mt(kmer_afters[x], new_unitig_idx);
            work_queue->add_work(1); 
          }
        }
      }
      for(int x = 0; x < 4; x++){
        if(candidates_before[x]){
          if(!startKmer2unitig.find_mt(kmer_befores_RC[x], idx)){
            int new_unitig_idx = contigs.push_back_mt(kmer_befores_RC[x]);
            startKmer2unitig[kmer_befores_RC[x]] = -new_unitig_idx;
            work_queue->add_work(1);
          }
        }
      }     
    }else if(candidates_before_num==1){
      for(int x = 0; x<4; x++){
        if(candidates_before[x]){
          current_kmer = kmer_befores[x];
          contig_seq = current_kmer.front()+contig_seq;
          break;
        }
      }
      continue;
    }else{
      startKmer2unitig.insert_mt(current_kmer, contig_id);
    }
    break;
  }
}
*/

/*
void get_unitig_backward(CQF_mt& cqf, const Params& options, vector_mt<Contig>& contigs, unordered_map_mt<string, int>& startKmer2unitig, WorkQueue* work_queue, int contig_id){
  auto abundance_min = options.kmer_abundance_min;
  auto K = options.K;

  array<bool, 4> candidates_before, candidates_after;
  //array<string, 4> kmer_befores, kmer_afters, kmer_befores_RC, kmer_afters_RC;
  int candidates_before_num, candidates_after_num;
  int nodes_before_num, nodes_after_num;
  uint64_t kmer_hash, kmer_RC_hash;
  string kmer, kmer_RC, current_kmer;
  uint64_t kmer_count, kmer_RC_count;                                                       
  int idx;

  string contig_seq = contigs.at_mt(contig_id); 
  current_kmer = contig_seq.substr(0, K);
  current_kmer_RC = RC_DNA(current_kmer);

  while(true){


    candidates_before = candidates_after = {{false, false, false, false}};
    //kmer_befores = kmers_before(current_kmer);
    //kmer_afters = kmers_after(kmer_befores[0]);
    candidates_before_num = candidates_after_num = 0;
    nodes_before_num = nodes_after_num = 0;

    for(int x = 0; x<4; x++){
      if(kmer_afters[x]==current_kmer){
        continue;
      }
      kmer = kmer_afters[x];
      kmer_RC = RC_DNA(kmer);
      kmer_afters_RC[x] = kmer_RC;

      NTPC64(kmer.c_str(), K, kmer_hash, kmer_RC_hash);
      if(kmer_hash > kmer_RC_hash){
        kmer_hash = kmer_RC_hash;
      }
      //kmer_hash = MurmurHash2(kmer);
      //kmer_RC_hash = MurmurHash2(kmer_RC);
      if(cqf.count_key_value_set_traveled(kmer_hash%cqf.qf->metadata->range, kmer_count)){
        if(startKmer2unitig.find_mt(kmer, idx)){
          nodes_after_num++;
        }else if(kmer_count >= abundance_min){
          candidates_after[x] = true;
          candidates_after_num ++;
        }
      }else if(kmer_count >= abundance_min){
        candidates_after[x] = true;
        candidates_after_num ++;        
      }
    }  
    for(int x = 0; x<4; x++){
      kmer = kmer_befores[x];
      kmer_RC = RC_DNA(kmer);
      kmer_befores_RC[x] = kmer_RC;

      NTPC64(kmer.c_str(), K, kmer_hash, kmer_RC_hash);
      if(kmer_hash > kmer_RC_hash){
        kmer_hash = kmer_RC_hash;
      }
      //kmer_hash = MurmurHash2(kmer);
      //kmer_RC_hash  = MurmurHash2(kmer_RC);
      if(cqf.count_key_value_set_traveled(kmer_hash%cqf.qf->metadata->range, kmer_count)){
        if(startKmer2unitig.find_mt(kmer_RC, idx)){
          nodes_before_num++; 
        }else if (kmer_count >= abundance_min){
          candidates_before[x] = true;
          candidates_before_num++;
        }
      }else if(kmer_count >= abundance_min){
        candidates_before[x] = true;
        candidates_before_num++;
      }
    }
    if((nodes_before_num + candidates_before_num)>1 || (nodes_after_num+candidates_after_num)){
      // if(startKmer2unitig.find_mt(RC_DNA(contig_seq.substr(contig_seq.length()-params.K)), idx)){
      //   //A contig has been constructed by another program from RC way
      //   if(abs(idx) != contig_id){
      //     contigs.set_mt(contig_id, ""); 
      //     break;
      //   }
      // }else{
      //   throw logic_error("Unexpectedly not found start kmer!");
      // }
      startKmer2unitig.insert_mt(current_kmer, contig_id);
      contigs.set_mt(contig_id, contig_seq);
      
      for(int x= 0; x < 4; x++){
        if(candidates_after[x]){
          if(!startKmer2unitig.find_mt(kmer_afters[x], idx)){
            int new_unitig_idx = contigs.push_back_mt(kmer_afters[x]);
            startKmer2unitig.insert_mt(kmer_afters[x], new_unitig_idx);
            work_queue->add_work(1); 
          }
        }
      }
      for(int x = 0; x < 4; x++){
        if(candidates_before[x]){
          if(!startKmer2unitig.find_mt(kmer_befores_RC[x], idx)){
            int new_unitig_idx = contigs.push_back_mt(kmer_befores_RC[x]);
            startKmer2unitig[kmer_befores_RC[x]] = -new_unitig_idx;
            work_queue->add_work(1);
          }
        }
      }
      break;     
    }else if(candidates_before_num==1){
      for(int x = 0; x<4; x++){
        if(candidates_before[x]){
          current_kmer = kmer_befores[x];
          contig_seq = current_kmer.front()+contig_seq;
          break;
        }
      }
      continue;
    }else{
      startKmer2unitig.insert_mt(current_kmer, contig_id);
      contigs.set_mt(contig_id, contig_seq);
      break;
    }
  }
}
*/

#if 0
void find_unitigs_hash_mt(CQF_mt& cqf, const Sequences& seqs, const Params& params, vector<string>& contigs){
  string unitig, kmer, kmer_RC;
  uint64_t count, kmer_hash, kmer_RC_hash;

  string seq_RC;

  vector_mt<string> unitigs;
  vector<Unitig_node> unitig_nodes;
  unordered_map_mt<string, int> startKmer2unitig(1ULL<<30);//, endKmer2unitig;

  int counter = 0;
  for(auto seq:seqs){
    if(seq.length()<params.K){
      continue;
    }
    //seq_RC = RC_DNA(seq);
    int seq_len = seq.length();
    int step = seq_len/3;
    for(int x = 0; x<=seq_len-params.K; x += step){
      unitigs.clear();
      unitig_nodes.clear();
      startKmer2unitig.clear(); endKmer2unitig.clear();   

      kmer = seq.substr(x, params.K);
      kmer_RC = RC_DNA(kmer);//seq_RC.substr(seq_len-params.K-x, params.K);
      kmer_hash = MurmurHash2(kmer);
      kmer_RC_hash = MurmurHash2(kmer_RC);
      
      //if(kmer=="CAGAACTCAAGGAAACACTTATGTTTACAGGT"){
      //  cout<<"Good!"<<endl;
      //}else{
      //  continue;
      //}

      if(cqf.is_traveled(kmer_hash)){
        continue;
      }else if(cqf.count(kmer_hash) < params.CONTIG_min_cov || cqf.count(kmer_RC_hash) < params.CONTIG_min_cov){
        cqf.set_traveled(kmer_hash);
        cqf.set_traveled(kmer_RC_hash);
        continue;
      }
      counter++;
      
      /*
      if(counter==14){
        fasta_write(contigs, params.OUT_CONTIG_min_len, dir+"counter13.fa");
      }
      if(contigs.size()>26046){
        cout<<counter<<endl;
        contig_summary(contigs);
        fasta_write(contigs, params.OUT_CONTIG_min_len, dir+"tmp.fa");
      }
      */

      cqf.set_traveled(kmer_hash);
      cqf.set_traveled(kmer_RC_hash);   
    
      startKmer2unitig[kmer] = 1;
      endKmer2unitig[kmer_RC] = -1;
      unitigs.resize(2);
      unitigs[1] = kmer;
      unitig_nodes.resize(2);

      get_unitig_forward1(cqf, params, unitigs, unitig_nodes, startKmer2unitig, endKmer2unitig, 1, kmer);
      get_unitig_backward1(cqf, params, unitigs, unitig_nodes, startKmer2unitig, endKmer2unitig, 1, kmer);
      int current_unitig_idx = 1, unitigs_before_num, unitigs_after_num;
      while(current_unitig_idx < unitigs.size()-1){
        current_unitig_idx++;
        unitigs_before_num = unitig_nodes[current_unitig_idx].unitigs_before.size();
        unitigs_after_num = unitig_nodes[current_unitig_idx].unitigs_after.size();
        
        if(unitigs_before_num>0 && unitigs_after_num==0){
          //check for existence of the same unitigs
          if(startKmer2unitig[unitigs[current_unitig_idx]]!=current_unitig_idx){
            for(auto ele:unitig_nodes[current_unitig_idx].unitigs_before){
              if(ele>0){
                unitig_nodes[ele].unitigs_after.erase(current_unitig_idx);
              }else{
                unitig_nodes[-ele].unitigs_before.erase(-current_unitig_idx); 
              }
            }
            unitig_nodes[current_unitig_idx].unitigs_before.clear();
            unitigs[current_unitig_idx] = "";
            continue;
          }
          get_unitig_forward1(cqf, params, unitigs, unitig_nodes, startKmer2unitig, endKmer2unitig, current_unitig_idx, unitigs[current_unitig_idx]);
        }else if(unitigs_before_num==0 && unitigs_after_num>0){
          //check for existence of the same unitigs
          if(endKmer2unitig[unitigs[current_unitig_idx]]!=current_unitig_idx){
            for(auto ele:unitig_nodes[current_unitig_idx].unitigs_after){
              if(ele>0){
                unitig_nodes[ele].unitigs_before.erase(current_unitig_idx);
              }else{
                unitig_nodes[-ele].unitigs_after.erase(-current_unitig_idx);
              }
            }
            unitig_nodes[current_unitig_idx].unitigs_after.clear();
            unitigs[current_unitig_idx] = "";
            continue;
          }
          get_unitig_backward1(cqf, params, unitigs, unitig_nodes, startKmer2unitig, endKmer2unitig, current_unitig_idx, unitigs[current_unitig_idx]);
        }else if(unitigs_before_num==0 && unitigs_after_num ==0){
          if(startKmer2unitig.find(unitigs[current_unitig_idx]) != startKmer2unitig.end()){
            //check for existence of the same unitigs
            if(startKmer2unitig[unitigs[current_unitig_idx]]!=current_unitig_idx){
              for(auto ele:unitig_nodes[current_unitig_idx].unitigs_before){
                if(ele>0){
                  unitig_nodes[ele].unitigs_after.erase(current_unitig_idx);
                }else{
                  unitig_nodes[-ele].unitigs_before.erase(-current_unitig_idx);  
                }
              }
              unitig_nodes[current_unitig_idx].unitigs_before.clear();
              unitigs[current_unitig_idx] = "";
              continue;
            }
            get_unitig_forward1(cqf, params, unitigs, unitig_nodes, startKmer2unitig, endKmer2unitig, current_unitig_idx, unitigs[current_unitig_idx]);
          }
          if(endKmer2unitig.find(unitigs[current_unitig_idx]) != endKmer2unitig.end()){
            //check for existence of the same unitigs
            if(endKmer2unitig[unitigs[current_unitig_idx]]!=current_unitig_idx){
              for(auto ele:unitig_nodes[current_unitig_idx].unitigs_after){
                if(ele>0){
                  unitig_nodes[ele].unitigs_before.erase(current_unitig_idx);
                }else{
                  unitig_nodes[-ele].unitigs_after.erase(-current_unitig_idx);
                }
              }
              unitig_nodes[current_unitig_idx].unitigs_after.clear();
              unitigs[current_unitig_idx] = "";
              continue;
            }
            get_unitig_backward1(cqf, params, unitigs, unitig_nodes, startKmer2unitig, endKmer2unitig, current_unitig_idx, unitigs[current_unitig_idx]);
          }
        }else{
          throw std::runtime_error("Unexpected cases: unextended unitig with non-zero before and after unitigs!");
        }
      }

      size_t unitig_num = unitigs.size();
      vector<bool> traveled(unitig_num, false); traveled[0]=true;
      
      /*
      cout<<"*"<<unitigs.size()<<" unitigs in total."<<endl;
      for(int x = 0; x< unitigs.size(); x++){
        cout<<"unitig "<<x<<endl;
        cout<<"\tseq: "<<unitigs[x]<<endl;
        cout<<"\tunitigs_before: ";
        for(auto ele:unitig_nodes[x].unitigs_before){
          cout<<ele<<" ";
        }
        cout<<endl;
        cout<<"\tunitigs_after: ";
        for(auto ele:unitig_nodes[x].unitigs_after){
          cout<<ele<<" ";
        }
        cout<<endl;
      }
      */
      remove_tips(params, unitigs, unitig_nodes, traveled);
      deduce_contigs(params, unitigs, unitig_nodes, contigs, traveled);
    }
  }
  /*
  cout<<"*"<<contigs.size()<<" contigs in total."<<endl;
  for(int x  = 0; x<contigs.size(); x++){
    cout<<"contig "<<x<<endl;
    cout<<"seq: "<<contigs[x]<<endl;
  }
  */
}


void ntHash(){
  string read = "ATGC";
  int K = 4;
  uint64_t hash, hash_RC;
  NTPC64(read.c_str(), K, hash, hash_RC);
  cout<<read<<endl;
  cout<<hash<<"\t"<<hash_RC<<endl;

  read = "atgc";
  NTPC64(read.c_str(), K, hash, hash_RC);
  cout<<read<<endl;
  cout<<hash<<"\t"<<hash_RC<<endl;
  
  read = "atgN";
  NTPC64(read.c_str(), K, hash, hash_RC);
  cout<<read<<endl;
  cout<<hash<<"\t"<<hash_RC<<endl;

  read = "atgn";
  NTPC64(read.c_str(), K, hash, hash_RC);
  cout<<read<<endl;
  cout<<hash<<"\t"<<hash_RC<<endl;
 
  read = "qftg";
  NTPC64(read.c_str(), K, hash, hash_RC);
  cout<<read<<endl;
  cout<<hash<<"\t"<<hash_RC<<endl;

  if(argc<3){
    cout<<"No enough parameters."<<endl;
    cout<<argv[0]<<" <K> <cqf2load>"<<endl;
  }

}
#endif
