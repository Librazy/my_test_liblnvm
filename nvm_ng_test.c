#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <liblightnvm.h>

const char *nvm_path = "/dev/nvme0n1";

struct nvm_dev *dev = NULL;
const struct nvm_geo *geo = NULL;
struct nvm_addr addr_lun;
struct nvm_addr addr_block;
struct nvm_addr addr_page;

/*
 * write/read - page
 * erase      - block of each plane of one LUN
 *
 * write/erase will NOT update bbt
 *
 */

void set_default_addr()
{
	addr_lun.ppa = 0;
	addr_lun.g.ch = 0;  //channel
	addr_lun.g.lun = 0; //lun
	
	addr_block.ppa = 0;
	addr_block.g.ch = 0;
	addr_block.g.lun = 0;
	addr_block.g.pl = 0;	//plane
	addr_block.g.blk = 0;	//block
	
	addr_page.ppa = 0;
	addr_page.g.ch = 0;
	addr_page.g.lun = 0;
	addr_page.g.pl = 0;
	addr_page.g.blk = 0;
	addr_page.g.pg = 0;	//page
}

void* thread_gc()
{
	printf("thread gc started\n");
	
	struct nvm_ret ret;
	int res = 0;

	const struct nvm_bbt* bbt = nvm_bbt_get(dev,addr_lun,&ret);
	if(!bbt){
		printf("[ERROR] get bbt in thread gc");
		return NULL;
	}
	struct nvm_bbt* bbt_cp = nvm_bbt_alloc_cp(bbt);	//copy bbt to modify
	
	int pmode = NVM_FLAG_PMODE_SNGL;
	const int naddrs = geo->nplanes;
	struct nvm_addr addrs[naddrs];
	
	for(size_t blk_idx = 0; blk_idx < bbt_cp->nblks; ++blk_idx){
		if(bbt_cp->blks[ blk_idx ] != NVM_BBT_BAD)
			continue;
		
		struct nvm_addr blk_addr;
		blk_addr.ppa = 0;
		blk_addr.g.ch = bbt_cp->addr.g.ch;
		blk_addr.g.lun = bbt_cp->addr.g.lun;
		blk_addr.g.pl = blk_idx % geo->nplanes;
		blk_addr.g.blk = blk_idx / geo->nplanes;
		
		printf("erase block\n");
		nvm_addr_pr(blk_addr);
		
		for (size_t pl = 0; pl < naddrs; ++pl) {	// Erase
			addrs[pl].ppa = blk_addr.ppa;
			addrs[pl].g.pl = pl;
		}
		
		res = nvm_addr_erase(dev, addrs, naddrs, pmode, &ret);
		//then the blocks of all plane will be erased
		//blocks which are on other planes should NOT by earsed again
		if (res < 0) {
			printf("[ERROR] erase in thread_gc\n");
			return NULL;
		}
		
		//updated bbt
		//bbt_cp->blks[ blk_idx ] = NVM_BBT_FREE;
		//set all the blocks to NVM_BBT_FREE
		int low_blk = blk_addr.g.blk * geo->nplanes;
		for(int i = 0; i < geo->nplanes; ++i){
			bbt_cp->blks[ low_blk + i ] = NVM_BBT_FREE;
		}
//		if(dev->bbts_cached){	//if cache is off, then it is better to update later
		//to maitain its consistency, update should be executed now, cache should be on
		res = nvm_bbt_set(dev,bbt_cp,&ret);
		if(res < 0){
			printf("[ERROR] update bbt in thread gc");
			return NULL;
		}
//		}
	}
	res = nvm_bbt_flush(dev,addr_lun,&ret);
	if(res < 0){
		printf("[ERROR] flush bbt in thread gc\n");
		return NULL;
	}
	
//	if(!bbt->bbts_cached){	//update bbt together at once
// 		res = nvm_bbt_set(dev,bbt,&ret);
//		if(res < 0){
//			printf("[ERROR] update bbt in thread gc");
//			return NULL;
//		}
//	}
	printf("thread gc ended\n");
}

void test_gc()
{	
	const struct nvm_bbt* bbt = NULL;
	struct nvm_ret ret;
	
	//check block state
	//notice that write in test_io() does not update the bbt
	bbt = nvm_bbt_get(dev,addr_lun,&ret);
	if(!bbt){
		printf("[ERROR] get bbt in test_gc 1\n");
		return;
	}
	printf("Number of block : %lu\n",bbt->nblks);
	printf("Number of bad   : %d\n",bbt->nbad);
	printf("Number of gbad  : %d\n",bbt->ngbad);
	printf("Number of dmrk  : %d\n",bbt->ndmrk);
	printf("Number of hmrk  : %d\n",bbt->nhmrk);
	int blk_idx = addr_block.g.pl * geo->nblocks + addr_block.g.blk;
	printf("before : ");
	nvm_bbt_state_pr(bbt->blks[ blk_idx ]);putchar('\n');
	
	//set block BAD
	struct nvm_bbt* bbt_cp = nvm_bbt_alloc_cp(bbt);
	if(!bbt_cp){
		printf("[ERROR] copy bbt in test_gc\n");
		return;
	}
	bbt_cp->blks[ blk_idx ] = NVM_BBT_BAD;
	int res = nvm_bbt_set(dev,bbt_cp,&ret);	//set bbt and refresh counters, according to bbt.addr
	/*	if cache is on, bbt cached by dev will be updated and will not be flushed into dev
	 *	if cache is off, bbt cached by dev will be updated, then flushed into dev,
	 *				and finally will be freed
	 */
	if(res < 0){
		printf("[ERROR] set bbt in test_gc\n");
		return;
	}
	
	//check block state again
	const struct nvm_bbt* bbt_after = nvm_bbt_get(dev,addr_lun,&ret);
	if(!bbt_after){
		printf("[ERROR] get bbt in test_gc 2\n");
		return;
	}
	printf("Number of block : %lu\n",bbt_after->nblks);
	printf("Number of bad   : %d\n",bbt_after->nbad);
	printf("Number of gbad  : %d\n",bbt_after->ngbad);
	printf("Number of dmrk  : %d\n",bbt_after->ndmrk);
	printf("Number of hmrk  : %d\n",bbt_after->nhmrk);
	printf("after  : ");
	nvm_bbt_state_pr(bbt_after->blks[ blk_idx ]);putchar('\n');
	
	//gc thread
	pthread_t gc_id = 0;
	int ret_thread_gc = pthread_create(&gc_id,NULL,thread_gc,NULL);
	if(ret_thread_gc){
		printf("[ERROR] create thread gc in test_gc\n");
		return;
	}
	printf("thread gc id : %lu\n",gc_id);
	pthread_join(gc_id, NULL);
	
	const struct nvm_bbt* bbt_after_gc = nvm_bbt_get(dev,addr_lun,&ret);
	if(!bbt_after_gc){
		printf("[ERROR] get bbt in thread gc 3\n");
		return;
	}
	printf("Number of block : %lu\n",bbt_after_gc->nblks);
	printf("Number of bad   : %d\n",bbt_after_gc->nbad);
	printf("Number of gbad  : %d\n",bbt_after_gc->ngbad);
	printf("Number of dmrk  : %d\n",bbt_after_gc->ndmrk);
	printf("Number of hmrk  : %d\n",bbt_after_gc->nhmrk);
	printf("after gc : ");
	nvm_bbt_state_pr(bbt_after_gc->blks[ blk_idx ]);putchar('\n');

	nvm_bbt_free(bbt_cp);
}

void test_io()
{
	ssize_t res;
	struct nvm_ret ret;
	char *content  = "hello nvme";
	
	//write
	char *buf_w = NULL;
	int buf_w_nbytes = geo->sector_nbytes;	//4096 bytes
	buf_w = nvm_buf_alloc(geo,buf_w_nbytes);	//aligned memory
	nvm_buf_fill(buf_w,buf_w_nbytes);
	sprintf(buf_w,"[test_io] %s",content);	
	
	int pmode = NVM_FLAG_PMODE_SNGL;
	res = nvm_addr_write(dev,&addr_page,1,buf_w,NULL,pmode,&ret);
	if(res < 0){
		printf("[ERROR] write in test_io\n");
		return;
	}
	
	printf("write: %s\n",buf_w);
	
	//read
	char *buf_r = NULL;
	int buf_r_nbytes = geo->sector_nbytes;
	buf_r = nvm_buf_alloc(geo,buf_r_nbytes);
	
	res = nvm_addr_read(dev,&addr_page,1,buf_r,NULL,pmode,&ret);
	if(res < 0){
		printf("[ERROR] write in test_io\n");
		return;
	}
	
	printf("read: %s\n",buf_r);
}

void test_bbt()
{
	const struct nvm_bbt* bbt = NULL;
	struct nvm_ret ret;
	bbt = nvm_bbt_get(dev,addr_lun,&ret);	//bbt will be cached between get and flush
	if(!bbt){
		printf("[ERROR] get bbt in test_bbt\n");
		return;
	}
	nvm_bbt_pr(bbt);
//	nvm_bbt_free(bbt);	//been cached, do not free
}

int main(int argc,char **argv)
{
	printf("------------------------------------------get dev\n");
	dev = nvm_dev_open(nvm_path);
	if(!dev){
		perror("[Error] nvm_dev_open");
		return 1;
	}
	nvm_dev_pr(dev);
	int res = nvm_dev_set_bbts_cached(dev,1);
	if(res < 0){
		printf("[ERROR] set bbts cached in main\n");
		return 1;
	}
	
	printf("------------------------------------------get geo\n");
	geo = nvm_dev_get_geo(dev);
	if(!geo){
		perror("[Error] nvm_get_geo");
		return 2;
	}
	nvm_geo_pr(geo);

	set_default_addr();
	
	printf("-----------------------------------------test bbt\n");
	test_bbt();

	printf("-----------------------------------------test io\n");
	test_io();

	printf("-----------------------------------------test gc\n");
	test_gc();

_OUT_MAIN_:
	nvm_dev_close(dev);
	return 0;
}
