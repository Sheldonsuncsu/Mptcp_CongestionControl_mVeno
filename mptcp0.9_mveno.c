/*
 * TCP Mveno congestion control
 * http://blog.csdn.net/zhangskd/article/details/7496670
 * http://blog.csdn.net/zhangskd/article/details/7303039
 */
#include <net/tcp.h>
#include <net/mptcp.h>
#include <linux/mm.h>
#include <linux/skbuff.h>
#include <linux/inet_diag.h>

#include <linux/module.h>

/* Default values of the Mveno variables, in fixed-point representation
 * with V_PARAM_SHIFT bits to the right of the binary point.
 */
#define V_PARAM_SHIFT 1
static const int beta = 3;
static const int n = 5;
static const int m = 2;

/* Mveno variables */
struct mveno {

	u32 beg_snd_nxt;     /* right edge during last rtt */
	u8  doing_mveno_now;/* if true, do Mveno for this rtt */
	u16 cntrtt;/* # of rtts measured within last rtt ,每收到一个 ACK就可以得到一个rtt样本，这个rtt内所收到的ACK个数，就是所能得到的上一个rtt的样本数*/
	u32 minrtt;/* min of one subfllow rtts measured within last rtt (in usec) 一个子流中取cntrtt个样本中最小者*/
        u32 sumrtt;      
        u32 cwnd;        
};


static inline int mptcp_mveno_sk_can_send(const struct sock *sk)
{
 
       return mptcp_sk_can_send(sk) && tcp_sk(sk)->srtt_us;
}

/* There are several situations when we must "re-start" Mveno:
 *
 *  o when a connection is established   第一个RTT没有上一个RTT，不能使用
 *  o after an RTO    超时后的第一个RTT，其上一个正常RTT是很久前的，不能使用
 *  o after fast recovery 快速恢复后的第一个RTT，其上一个正常RTT也是很久之前
 *  o when we send a packet and there is no outstanding
 *    unacknowledged data (restarting an idle connection)
 *   idle后发数据，上个RTT较久之前的，不能使用
 */
static inline void mveno_enable(struct sock *sk)
{
	const struct tcp_sock *tp = tcp_sk(sk);
	struct mveno *mveno = inet_csk_ca(sk);

	/* turn on mveno */
	mveno->doing_mveno_now = 1;
	mveno->beg_snd_nxt = tp->snd_nxt;

	mveno->minrtt = 0x7fffffff;/* 设成最大值 */
	mveno->cntrtt = 0; /* RTT计数器清零，这个才是控制是否使用mveno关键！ */
	mveno->sumrtt = 0;
	mveno->cwnd = tp->snd_cwnd;

}


static inline void mveno_disable(struct sock *sk)
{
	struct mveno *mveno = inet_csk_ca(sk);

	/* turn off Veno */
	mveno->doing_mveno_now = 0;

}


static void mptcp_mveno_init(struct sock *sk)
{ 
       if(tcp_sk(sk)->mpc)
	 mveno_enable(sk);

 
      /* If we do not mptcp, behave like reno: return */
}

static void mptcp_mveno_state(struct sock *sk, u8 ca_state)
{	
	if (!tcp_sk(sk)->mpc)
		return;
	if (ca_state == TCP_CA_Open)
		mveno_enable(sk);
	else
		mveno_disable(sk);


}

static void mptcp_mveno_cwnd_event(struct sock *sk, enum tcp_ca_event event)
{
	if (event == CA_EVENT_CWND_RESTART || event == CA_EVENT_TX_START)
		mptcp_mveno_init(sk);

  
}


/*  每收到一个ACK都会调用此函数  */
/* Do rtt sampling needed for MVeno. */
static void mptcp_mveno_pkts_acked(struct sock *sk, u32 cnt, s32 rtt_us)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct mveno *mveno = inet_csk_ca(sk);
	u32 vrtt;
/* 只是定义变量，并没有用它的意义  */

	if (rtt_us < 0)
		return;

	/* Never allow zero rtt or baseRTT */
	vrtt = rtt_us + 1;


   /* Filter to find propagation delay: */

	/* Find the min rtt during the last rtt to find
	 * the current prop. delay + queuing delay:
	 */
      mveno->minrtt = min(mveno->minrtt, vrtt);
      mveno->sumrtt += vrtt;
	mveno->cntrtt++;

        if(tp->snd_cwnd > mveno->cwnd){
       
                mveno->cwnd = tp->snd_cwnd;
        }

 
}

/*更新baseRTT，连接中所有子流中的最小RTT*/

static u32 mptcp_mveno_recalc_baseRTT(struct sock *sk)
{
      struct mptcp_cb *mpcb = tcp_sk(sk)->mpcb;
	struct sock *sub_sk;
	u32 baseRTT = 0x7fffffff;

	mptcp_for_each_sk(mpcb, sub_sk) {

		struct mveno *mveno = inet_csk_ca(sub_sk);
		if (!mptcp_mveno_sk_can_send(sub_sk))
			continue;

             if (mveno->minrtt < baseRTT)
                 baseRTT = mveno->minrtt;

	}

        printk(KERN_EMERG "this is recalc baseRTT baseRTT :%u\n",baseRTT);

	return baseRTT;
}

/*更新子流diff */

static u32 mptcp_mveno_recalc_diff(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct mveno *mveno = inet_csk_ca(sk);
	u32 rtt = 0x7fffffff;
        u32 diff = 0;
	if (mveno->cntrtt==0)
         rtt = mveno->sumrtt;
	else
	   rtt = mveno->sumrtt/mveno->cntrtt;
	if (rtt==0)
	  diff = tp->snd_cwnd*(rtt-mveno->minrtt);
	else
        diff = tp->snd_cwnd*(rtt-mveno->minrtt)/rtt;

	return diff;
}

/*更新Ts，整个连接所有子流的平均RTT*/

static u32 mptcp_mveno_recalc_averageRTT(struct sock *sk)
{
      struct mptcp_cb *mpcb = tcp_sk(sk)->mpcb;        
	struct sock *sub_sk;

	u32 averageRTT = 0x7fffffff;
      u32 sumRTT = 0;
      u32 cnt = 0;

	mptcp_for_each_sk(mpcb,sub_sk) {

		struct mveno *mveno = inet_csk_ca(sub_sk);
		if (!mptcp_mveno_sk_can_send(sub_sk))
			continue;

                sumRTT += mveno->sumrtt;
                cnt+=mveno->cntrtt;
 
	}
	  if (cnt==0)
	  averageRTT = sumRTT/(cnt+1);
	  else
        averageRTT = sumRTT/cnt;
        
	return averageRTT;
}
static u32 mptcp_mveno_recalc_ps(struct sock *sk,u32 baseRTT,u32 averageRTT)
{
	struct mptcp_cb *mpcb = tcp_sk(sk)->mpcb;        
	struct sock *sub_sk;
	u32 cwnd = 0;
	u32 ps;

	mptcp_for_each_sk(mpcb,sub_sk) {

		struct tcp_sock *sub_tp = tcp_sk(sub_sk);
		if (!mptcp_mveno_sk_can_send(sub_sk))
			continue;

                if(sub_tp->snd_cwnd > cwnd){
                        cwnd = sub_tp->snd_cwnd;
                }/*计算最大发送窗口*/
	}

        if (averageRTT != baseRTT){
		  if((averageRTT*cwnd-baseRTT*cwnd)==0)
	        ps = beta*averageRTT/(averageRTT*cwnd-baseRTT*cwnd+1);
		
		  else
                ps = beta*averageRTT/(averageRTT*cwnd-baseRTT*cwnd);
		
        }
        else 
                ps = 1;

	ps=1-min(1,ps);
/*
	
	       if (es > 1/n)
		
	        return es;
        else {
		if (es==0)
		{ es=0.5;
		
		return es;
		}
		
                return 1/n;}
*/
        return ps;
}

static u32 mptcp_mveno_recalc_psr(struct sock *sk)
{
	struct mveno *mveno = inet_csk_ca(sk);

        u32 rtt;
        u32 psr;  
	
	 if(mveno->cntrtt==0)
		rtt = mveno->sumrtt/(mveno->cntrtt+1);
	 else
            rtt = mveno->sumrtt/mveno->cntrtt;

        if (rtt != mveno->minrtt){
		   if (rtt*mveno->cwnd-mveno->minrtt*mveno->cwnd==0)
			psr = beta*rtt/(rtt*mveno->cwnd-mveno->minrtt*mveno->cwnd+1);
			
		   else
                        psr = beta*rtt/(rtt*mveno->cwnd-mveno->minrtt*mveno->cwnd);
		
        }
        else 
                psr = 1;
	psr=1-min(1,psr);
	return psr;

 /*      
       if (esr > 1/n)
	        return esr;
        else {
		if (esr==0)
		{ esr=0.5;
		return esr;
		}
		
                return 1/n;}

*/
         
}


/*更新Es*/

static u32 mptcp_mveno_recalc_es(struct sock *sk,u32 baseRTT,u32 averageRTT)
{
	struct mptcp_cb *mpcb = tcp_sk(sk)->mpcb;        
	struct sock *sub_sk;
	u32 cwnd = 0;
	u32 es; 

	mptcp_for_each_sk(mpcb,sub_sk) {

		struct tcp_sock *sub_tp = tcp_sk(sub_sk);
		if (!mptcp_mveno_sk_can_send(sub_sk))
			continue;

                if(sub_tp->snd_cwnd > cwnd){
                        cwnd = sub_tp->snd_cwnd;
                }/*计算最大发送窗口*/
	}

        if (averageRTT != baseRTT){
		  if((averageRTT*cwnd-baseRTT*cwnd)==0)
	        es = 1/2-(1/2-1/n)*beta*averageRTT/(averageRTT*cwnd-baseRTT*cwnd+1);
		
		  else
                es = 1/2-(1/2-1/n)*beta*averageRTT/(averageRTT*cwnd-baseRTT*cwnd);
		
        }
        else 
                es = 1/n;

	
	       if (es > 1/n)
		
	        return es;
        else {
		if (es==0)
		{ es=0.5;
		
		return es;
		}
		
                return 1/n;}

}

/*更新Es,r*/

static u32 mptcp_mveno_recalc_esr(struct sock *sk)
{
	struct mveno *mveno = inet_csk_ca(sk);

        u32 rtt;
        u32 esr;  
	
	 if(mveno->cntrtt==0)
		rtt = mveno->sumrtt/(mveno->cntrtt+1);
	 else
            rtt = mveno->sumrtt/mveno->cntrtt;

        if (rtt != mveno->minrtt){
		   if (rtt*mveno->cwnd-mveno->minrtt*mveno->cwnd==0)
			esr = 1/2-(1/2-1/n)*beta*rtt/(rtt*mveno->cwnd-mveno->minrtt*mveno->cwnd+1);
			
		   else
                esr = 1/2-(1/2-1/n)*beta*rtt/(rtt*mveno->cwnd-mveno->minrtt*mveno->cwnd);
		
        }
        else 
                esr = 1/n;

       
       if (esr > 1/n)
	        return esr;
        else {
		if (esr==0)
		{ esr=0.5;
		return esr;
		}
		
                return 1/n;}


}

/*更新Xs,r  子流吞吐量*/
static u32 mptcp_mveno_recalc_xsr(struct sock *sk)

{
      struct mptcp_cb *mpcb = tcp_sk(sk)->mpcb;      
	struct sock *sub_sk;
        u32 xsr;
	

      mptcp_for_each_sk(mpcb,sub_sk) {

		struct tcp_sock *sub_tp = tcp_sk(sub_sk);
		struct mveno *mveno = inet_csk_ca(sub_sk);

		if (!mptcp_mveno_sk_can_send(sub_sk))

			continue;


		    if(mveno->cntrtt==0)
			xsr = (sub_tp->snd_cwnd/(mveno->cntrtt+1))*(sub_tp->snd_cwnd/(mveno->cntrtt+1));
			
		    else
                xsr = (sub_tp->snd_cwnd/mveno->cntrtt)*(sub_tp->snd_cwnd/mveno->cntrtt);
		
	}

        
        return xsr;

}
static u32 mptcp_mveno_recalc_max_xsr(struct sock *sk)

{
      struct mptcp_cb *mpcb = tcp_sk(sk)->mpcb;      
	struct sock *sub_sk;
      u32 xsr;
	u32 max_xsr = 0;

      mptcp_for_each_sk(mpcb,sub_sk) {

		struct tcp_sock *sub_tp = tcp_sk(sub_sk);
		struct mveno *mveno = inet_csk_ca(sub_sk);

		if (!mptcp_mveno_sk_can_send(sub_sk))

			continue;


		    if(mveno->cntrtt==0)
			 xsr = (sub_tp->snd_cwnd/(mveno->cntrtt+1))*(sub_tp->snd_cwnd/(mveno->cntrtt+1));
		    else
                xsr = (sub_tp->snd_cwnd/mveno->cntrtt)*(sub_tp->snd_cwnd/mveno->cntrtt);

		if(xsr>max_xsr)

			max_xsr = xsr;


	}

      

        return max_xsr;

}


/*更新Ys  ，整个连接吞吐量*/
static u32 mptcp_mveno_recalc_ys(struct sock *sk)
{
      struct mptcp_cb *mpcb = tcp_sk(sk)->mpcb;          
	struct sock *sub_sk;

        u32 cwnd;
        u32 rtt;
        u32 ys;
	

        cwnd = 0;
        rtt = 0;

	mptcp_for_each_sk(mpcb,sub_sk) {

		struct tcp_sock *sub_tp = tcp_sk(sub_sk);
		struct mveno *mveno = inet_csk_ca(sub_sk);
		if (!mptcp_mveno_sk_can_send(sub_sk))
			continue;

                cwnd += sub_tp->snd_cwnd;
                rtt += mveno->sumrtt;
	}
	       
	  if (rtt==0)
	 ys = (cwnd/(rtt+1))*(cwnd/(rtt+1));
	  else
        ys = (cwnd/rtt)*(cwnd/rtt);
        return ys;

}

/*
 * If the connection is idle and we are restarting,
 * then we don't want to do any Veno calculations
 * until we get fresh rtt samples.  So when we
 * restart, we reset our Veno state to a clean
 * state. After we get acks for this flight of
 * packets, _then_ we can make Veno calculations
 * again.
 */

static void mptcp_mveno_cong_avoid(struct sock *sk, u32 ack, u32 in_flight)
{

       printk(KERN_EMERG "this is cong avoid;\n");        
  
	struct tcp_sock *tp = tcp_sk(sk);
      struct mptcp_cb *mpcb = tp->mpcb;
	struct mveno *mveno = inet_csk_ca(sk);
	  int snd_cwnd;
        u32 baseRTT;
        u32 diff;
        u32 averageRTT;
	u32 ps;
	u32 psr;
        u32 es;
        u32 esr=1;
        u32 xsr;
        u32 ys=1;
        u32 max_xsr=1;
        u32 snd_cwnd_more;
        u32 gamma;
        u32 theta;

        gamma = 1;
        theta = 1;
        diff = 3;





	/*如果mveno不可用，退回到reno*/
       if (!tp->mpc) {
		tcp_reno_cong_avoid(sk, ack, in_flight);
		return;
	}

	if (!mveno->doing_mveno_now) {
		tcp_reno_cong_avoid(sk, ack, in_flight);
		return;
	}
 	/* limited by applications */
	if (!tcp_is_cwnd_limited(sk))
		return;

     
        

	if (after(ack, mveno->beg_snd_nxt)) {/*本RTT结束了*/
		/* Do the Mveno once-per-RTT cwnd adjustment. */
		/* Save the extent of the current window so we can use this
		 * at the end of the next RTT.
		 */
               mveno->beg_snd_nxt  = tp->snd_nxt;/*设置新的RTT结束标志*/
                
	         /* Wipe the slate clean for the next rtt. */
	     
	   if (mveno->cntrtt <= 2) {
		/* We don't have enough rtt samples to do the Veno
		 * calculation, so we'll behave like Reno.
		 */
		 tcp_reno_cong_avoid(sk, ack, in_flight);

             
	  } else {
                

			/*每个RTT更新参数*/
      
                baseRTT= mptcp_mveno_recalc_baseRTT(sk);
                diff= mptcp_mveno_recalc_diff(sk);
                averageRTT = mptcp_mveno_recalc_averageRTT(sk);
		ps = mptcp_mveno_recalc_ps(sk,baseRTT,averageRTT);
		psr=mptcp_mveno_recalc_psr(sk);               
		es= mptcp_mveno_recalc_es(sk,baseRTT,averageRTT);
                esr= mptcp_mveno_recalc_esr(sk);
		xsr=mptcp_mveno_recalc_xsr(sk);
                max_xsr = mptcp_mveno_recalc_max_xsr(sk);
                ys= mptcp_mveno_recalc_ys(sk);
               
       

		if(mpcb->cnt_established >1){

			if(max_xsr==0)
			gamma = ys/(max_xsr+1);
			else
 		       gamma = ys/max_xsr;
			
		if(esr==0||mveno->sumrtt==0||mveno->cntrtt==0||xsr==0)
			  theta = es*averageRTT*ys;
				
			
		else
                theta = (es*averageRTT*ys)/(esr*(mveno->sumrtt/mveno->cntrtt)*(mveno->sumrtt/mveno->cntrtt)*xsr);
		
		
                 if(gamma > theta)
                            snd_cwnd = gamma;
                 else
                            snd_cwnd = theta;
		      if (snd_cwnd==0)
		      snd_cwnd_more = m/(snd_cwnd+1);
		      else
                      snd_cwnd_more = m/snd_cwnd;
                }
            else{
          
                       gamma = 1;
                       theta = 1;
  			    snd_cwnd = tp->snd_cwnd;
                       snd_cwnd_more = tp->snd_cwnd;
                }

               
         
		if (tp->snd_cwnd <= tp->snd_ssthresh) {
			/* Slow start.  */
			tcp_slow_start(tp,in_flight);

		} else {
			/* Congestion avoidance. */

			if (diff < beta) {
                                if (tp->snd_cwnd_cnt >= snd_cwnd) {
					        if (tp->snd_cwnd < tp->snd_cwnd_clamp) 
						       tp->snd_cwnd++;
					             else
					          tp->snd_cwnd_cnt = 0;
				       } else
					      tp->snd_cwnd_cnt++;
				
			} else {
				     if (tp->snd_cwnd_cnt >= snd_cwnd_more) {
					      if (tp->snd_cwnd < tp->snd_cwnd_clamp)
						tp->snd_cwnd++;	
					      else
					      tp->snd_cwnd_cnt = 0;
				     } else
					tp->snd_cwnd_cnt++;
			}

		}/*样本数大于2，拥塞控制阶段*/
		/*设置snd_cwnd的最小值和最大值*/
		if (tp->snd_cwnd < 2)
			tp->snd_cwnd = 2;
		else if (tp->snd_cwnd > tp->snd_cwnd_clamp)
			tp->snd_cwnd = tp->snd_cwnd_clamp;
		/*当snd_cwnd有了较大增长时，适当增加阈值*/
		
	    }  /*样本数大于2内的操作结束*/
		
                 mveno->cntrtt = 0;  //计数清零
                 mveno->sumrtt = 0;
                 mveno->minrtt = 0x7fffffff;
	}/*本RTT结束*/
	
	/*慢启动期间非RTT结束ACK*/
 else if(tp->snd_cwnd <= tp->snd_ssthresh) {
			/* Slow start.  */
			tcp_slow_start(tp,in_flight);
//         printk(KERN_EMERG "this is cong avoid 非RTT结束ACK 调用 slow start: %u\n");
	} 

/*对于拥塞避免期间非RTT结束ACK，不做任何处理 */             
        

}

/* Veno MD phase */
static u32 mptcp_mveno_ssthresh(struct sock *sk)
{

//      printk(KERN_EMERG "this is tcp mveno ssthresh;/n");
	const struct tcp_sock *tp = tcp_sk(sk);
      u32 diff;
      diff = mptcp_mveno_recalc_diff(sk);

	if (diff < beta)
		/* in "non-congestive state", cut cwnd by 1/5,如果判断为随机丢包，那么慢启动阈值减少幅度为0.2 */
		return max(tp->snd_cwnd * 4 / 5, 2U);
	else
		/* in "congestive state", cut cwnd by 1/2 ，如果判断为拥塞丢包，那么慢启动阈值减小幅度为0.5*/
		return max(tp->snd_cwnd >> 1U, 2U);
}

static struct tcp_congestion_ops mptcp_mveno = {
	.init		= mptcp_mveno_init,
	.ssthresh	= mptcp_mveno_ssthresh,
	.cong_avoid	= mptcp_mveno_cong_avoid,
	.pkts_acked	= mptcp_mveno_pkts_acked,
	.set_state	= mptcp_mveno_state,
	.cwnd_event	= mptcp_mveno_cwnd_event,

	.owner		= THIS_MODULE,
	.name		= "mveno",
};

static int __init mptcp_mveno_register(void)
{
	BUILD_BUG_ON(sizeof(struct mveno) > ICSK_CA_PRIV_SIZE);
	return tcp_register_congestion_control(&mptcp_mveno); 
	return 0;
}

static void __exit mptcp_mveno_unregister(void)
{
	tcp_unregister_congestion_control(&mptcp_mveno);
}

module_init(mptcp_mveno_register);
module_exit(mptcp_mveno_unregister);

MODULE_AUTHOR(".....");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MPTCP COUPLED CONGESTION CONTROL");
MODULE_VERSION("1.0");





