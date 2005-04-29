/*
 * lftp - file transfer program
 *
 * Copyright (c) 2000-2002 by Alexander V. Lukyanov (lav@yars.free.net)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <config.h>

#if USE_SSL
#include "lftp_ssl.h"
#include "xmalloc.h"
#include "ResMgr.h"
#include "log.h"
#include "misc.h"

lftp_ssl_base::lftp_ssl_base(int fd1,handshake_mode_t m,const char *h)
{
   fd=fd1;
   hostname=xstrdup(h);
   handshake_done=false;
   handshake_mode=m;
   error=0;
   fatal=false;
}
lftp_ssl_base::~lftp_ssl_base()
{
   xfree(hostname);
   xfree(error);
}
void lftp_ssl_base::set_error(const char *s1,const char *s2)
{
   xfree(error);
   error=(char*)xmalloc(xstrlen(s1)+2+xstrlen(s2)+1);
   if(s1)
   {
      strcpy(error,s1);
      strcat(error,": ");
      strcat(error,s2);
   }
   else
      strcpy(error,s2);
}

#if USE_GNUTLS

static void lftp_ssl_init()
{
   static bool inited=false;
   if(inited) return;
   inited=true;

   gnutls_global_init();
}
lftp_ssl_gnutls::lftp_ssl_gnutls(int fd1,handshake_mode_t m,const char *h)
   : lftp_ssl_base(fd1,m,h)
{
   lftp_ssl_init();

   gnutls_init(&session,(m==CLIENT?GNUTLS_CLIENT:GNUTLS_SERVER));
   gnutls_set_default_priority(session);

   gnutls_certificate_allocate_credentials(&cred);
//    gnutls_certificate_set_x509_trust_file(cred,CAFILE,GNUTLS_X509_FMT_PEM);
   gnutls_credentials_set(session, GNUTLS_CRD_CERTIFICATE, cred);

   gnutls_transport_set_ptr(session,(gnutls_transport_ptr_t)fd);
}
lftp_ssl_gnutls::~lftp_ssl_gnutls()
{
   gnutls_certificate_free_credentials(cred);
   gnutls_deinit(session);
}

int lftp_ssl_gnutls::do_handshake()
{
   if(handshake_done)
      return DONE;
   int res=gnutls_handshake(session);
   if(res<0)
   {
      if(res==GNUTLS_E_AGAIN || res==GNUTLS_E_INTERRUPTED)
	 return RETRY;
      else // error
      {
	 fatal=gnutls_error_is_fatal(res);
	 set_error("gnutls_handshake",gnutls_strerror(res));
	 return ERROR;
      }
   }
   handshake_done=true;
   return DONE;
}
int lftp_ssl_gnutls::read(char *buf,int size)
{
   int res=do_handshake();
   if(res!=DONE)
      return res;
   res=gnutls_record_recv(session,buf,size);
   if(res<0)
   {
      if(res==GNUTLS_E_AGAIN || res==GNUTLS_E_INTERRUPTED)
	 return RETRY;
      else // error
      {
	 fatal=gnutls_error_is_fatal(res);
	 set_error("gnutls_record_recv",gnutls_strerror(res));
	 return ERROR;
      }
   }
   return res;
}
int lftp_ssl_gnutls::write(const char *buf,int size)
{
   int res=do_handshake();
   if(res!=DONE)
      return res;
   res=gnutls_record_send(session,buf,size);
   if(res<0)
   {
      if(res==GNUTLS_E_AGAIN || res==GNUTLS_E_INTERRUPTED)
	 return RETRY;
      else // error
      {
	 fatal=gnutls_error_is_fatal(res);
	 set_error("gnutls_record_send",gnutls_strerror(res));
	 return ERROR;
      }
   }
   return res;
}
bool lftp_ssl_gnutls::want_in()
{
   return gnutls_record_get_direction(session)==0;
}
bool lftp_ssl_gnutls::want_out()
{
   return gnutls_record_get_direction(session)==1;
}
void lftp_ssl_gnutls::copy_sid(const lftp_ssl_gnutls *o)
{
   size_t session_data_size;
   void *session_data;
   gnutls_session_get_data(o->session,NULL,&session_data_size);
   session_data=xmalloc(session_data_size);
   gnutls_session_get_data(o->session,session_data,&session_data_size);
   gnutls_session_set_data(session,session_data,session_data_size);
}

/*=============================== OpenSSL ====================================*/
#elif USE_OPENSSL
static int lftp_ssl_verify_callback(int ok,X509_STORE_CTX *ctx);
static int lftp_ssl_verify_crl(X509_STORE_CTX *ctx);
//static int lftp_ssl_passwd_callback(char *buf,int size,int rwflag,void *userdata);

SSL_CTX *ssl_ctx;
X509_STORE *crl_store;

static char file[256];

static void lftp_ssl_write_rnd()
{
   RAND_write_file(file);
}

static void lftp_ssl_init()
{
   static bool inited=false;
   if(inited) return;
   inited=true;

#ifdef WINDOWS
   RAND_screen();
#endif

   RAND_file_name(file,sizeof(file));
   if(RAND_egd(file)>0)
      return;

   if(RAND_load_file(file,-1) && RAND_status()!=0)
      atexit(lftp_ssl_write_rnd);
}

static void lftp_ssl_ctx_init()
{
   if(ssl_ctx) return;

#if SSLEAY_VERSION_NUMBER < 0x0800
   ssl_ctx=SSL_CTX_new();
   X509_set_default_verify_paths(ssl_ctx->cert);
#else
   SSLeay_add_ssl_algorithms();
   ssl_ctx=SSL_CTX_new(SSLv23_client_method());
   SSL_CTX_set_options(ssl_ctx, SSL_OP_ALL);
   SSL_CTX_set_verify(ssl_ctx,SSL_VERIFY_PEER,lftp_ssl_verify_callback);
//    SSL_CTX_set_default_passwd_cb(ssl_ctx,lftp_ssl_passwd_callback);

   const char *ca_file=ResMgr::Query("ssl:ca-file",0);
   const char *ca_path=ResMgr::Query("ssl:ca-path",0);
   if(ca_file && !*ca_file)
      ca_file=0;
   if(ca_path && !*ca_path)
      ca_path=0;
   if(ca_file || ca_path)
   {
      if(!SSL_CTX_load_verify_locations(ssl_ctx,ca_file,ca_path))
      {
	 fprintf(stderr,"WARNING: SSL_CTX_load_verify_locations(%s,%s) failed\n",
	    ca_file?ca_file:"NULL",
	    ca_path?ca_path:"NULL");
	 SSL_CTX_set_default_verify_paths(ssl_ctx);
      }
   }
   else
   {
      SSL_CTX_set_default_verify_paths(ssl_ctx);
   }

   const char *crl_file=ResMgr::Query("ssl:crl-file",0);
   const char *crl_path=ResMgr::Query("ssl:crl-path",0);
   if(crl_file && !*crl_file)
      crl_file=0;
   if(crl_path && !*crl_path)
      crl_path=0;
   if(crl_file || crl_path)
   {
      crl_store=X509_STORE_new();
      if(!X509_STORE_load_locations(crl_store,crl_file,crl_path))
      {
	 fprintf(stderr,"WARNING: X509_STORE_load_locations(%s,%s) failed\n",
	    crl_file?crl_file:"NULL",
	    crl_path?crl_path:"NULL");
      }
   }
#endif /* SSLEAY_VERSION_NUMBER < 0x0800 */
}

lftp_ssl_openssl::lftp_ssl_openssl(int fd1,handshake_mode_t m,const char *h)
   : lftp_ssl_base(fd1,m,h)
{
   lftp_ssl_init();
   lftp_ssl_ctx_init();

   ssl=SSL_new(ssl_ctx);
   SSL_set_fd(ssl,fd);
   SSL_ctrl(ssl,SSL_CTRL_MODE,SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER,0);

   const char *key_file =ResMgr::Query("ssl:key-file",hostname);
   const char *cert_file=ResMgr::Query("ssl:cert-file",hostname);
   if(key_file && !*key_file)
      key_file=0;
   if(cert_file && !*cert_file)
      cert_file=0;

   if(cert_file)
   {
      if(!key_file)
	 key_file=cert_file;
      if(SSL_use_certificate_file(ssl,cert_file,SSL_FILETYPE_PEM)<=0)
      {
	 // FIXME
      }
      if(SSL_use_PrivateKey_file(ssl,key_file,SSL_FILETYPE_PEM)<=0)
      {
	 // FIXME
      }
      if(!SSL_check_private_key(ssl))
      {
	 // FIXME
      }
   }
}
lftp_ssl_openssl::~lftp_ssl_openssl()
{
   SSL_free(ssl);
}

static const char *host;
static int lftp_ssl_connect(SSL *ssl,const char *h)
{
   host=h;
   int res=SSL_connect(ssl);
   host=0;
   return res;
}
bool lftp_ssl_openssl::check_fatal(int res)
{
   return !(SSL_get_error(ssl,res)==SSL_ERROR_SYSCALL
	    && (ERR_get_error()==0 || temporary_network_error(errno)));
}

int lftp_ssl_openssl::do_handshake()
{
   if(SSL_is_init_finished(ssl))
      return DONE;
   if(handshake_mode==SERVER)
   {
      // FIXME: SSL_accept
      return RETRY;
   }
   errno=0;
   int res=lftp_ssl_connect(ssl,hostname);
   if(res<=0)
   {
      if(BIO_sock_should_retry(res))
	 return RETRY;
      else if (SSL_want_x509_lookup(ssl))
	 return RETRY;
      else // error
      {
	 fatal=check_fatal(res);
	 set_error("SSL_connect",strerror());
	 return ERROR;
      }
   }
   return DONE;
}
int lftp_ssl_openssl::read(char *buf,int size)
{
   int res=do_handshake();
   if(res!=DONE)
      return res;
   errno=0;
   res=SSL_read(ssl,buf,size);
   if(res<0)
   {
      if(BIO_sock_should_retry(res))
	 return RETRY;
      else if (SSL_want_x509_lookup(ssl))
	 return RETRY;
      else // error
      {
	 fatal=check_fatal(res);
	 set_error("SSL_read",strerror());
	 return ERROR;
      }
   }
   return res;
}
int lftp_ssl_openssl::write(const char *buf,int size)
{
   int res=do_handshake();
   if(res!=DONE)
      return res;
   errno=0;
   res=SSL_write(ssl,buf,size);
   if(res<0)
   {
      if(BIO_sock_should_retry(res))
	 return RETRY;
      else if (SSL_want_x509_lookup(ssl))
	 return RETRY;
      else // error
      {
	 fatal=check_fatal(res);
	 set_error("SSL_write",strerror());
	 return ERROR;
      }
   }
   return res;
}
bool lftp_ssl_openssl::want_in()
{
   return SSL_want_read(ssl);
}
bool lftp_ssl_openssl::want_out()
{
   return SSL_want_write(ssl);
}
void lftp_ssl_openssl::copy_sid(const lftp_ssl_openssl *o)
{
   SSL_copy_session_id(ssl,o->ssl);
}

static int certificate_verify_error;

const char *lftp_ssl_openssl::strerror()
{
   SSL_load_error_strings();
   int error=ERR_get_error();
   const char *ssl_error=0;
   if(ERR_GET_LIB(error)==ERR_LIB_SSL
   && ERR_GET_REASON(error)==SSL_R_CERTIFICATE_VERIFY_FAILED)
      ssl_error=X509_verify_cert_error_string(certificate_verify_error);
   else if(ERR_GET_LIB(error)==ERR_LIB_SSL)
      ssl_error=ERR_reason_error_string(error);
   else
      ssl_error=ERR_error_string(error,NULL);
   if(!ssl_error)
      ssl_error="error";
   return ssl_error;
}

/* This one is (very much!) based on work by Ralf S. Engelschall <rse@engelschall.com>.
 * Comments by Ralf. */
static int lftp_ssl_verify_crl(X509_STORE_CTX *ctx)
{
    X509_OBJECT obj;
    X509_NAME *subject;
    X509_NAME *issuer;
    X509 *xs;
    X509_CRL *crl;
    X509_REVOKED *revoked;
    X509_STORE_CTX store_ctx;
    long serial;
    int i, n, rc;
    char *cp;

    /*
     * Unless a revocation store for CRLs was created we
     * cannot do any CRL-based verification, of course.
     */
    if (!crl_store)
        return 1;

    /*
     * Determine certificate ingredients in advance
     */
    xs      = X509_STORE_CTX_get_current_cert(ctx);
    subject = X509_get_subject_name(xs);
    issuer  = X509_get_issuer_name(xs);

    /*
     * OpenSSL provides the general mechanism to deal with CRLs but does not
     * use them automatically when verifying certificates, so we do it
     * explicitly here. We will check the CRL for the currently checked
     * certificate, if there is such a CRL in the store.
     *
     * We come through this procedure for each certificate in the certificate
     * chain, starting with the root-CA's certificate. At each step we've to
     * both verify the signature on the CRL (to make sure it's a valid CRL)
     * and it's revocation list (to make sure the current certificate isn't
     * revoked).  But because to check the signature on the CRL we need the
     * public key of the issuing CA certificate (which was already processed
     * one round before), we've a little problem. But we can both solve it and
     * at the same time optimize the processing by using the following
     * verification scheme (idea and code snippets borrowed from the GLOBUS
     * project):
     *
     * 1. We'll check the signature of a CRL in each step when we find a CRL
     *    through the _subject_ name of the current certificate. This CRL
     *    itself will be needed the first time in the next round, of course.
     *    But we do the signature processing one round before this where the
     *    public key of the CA is available.
     *
     * 2. We'll check the revocation list of a CRL in each step when
     *    we find a CRL through the _issuer_ name of the current certificate.
     *    This CRLs signature was then already verified one round before.
     *
     * This verification scheme allows a CA to revoke its own certificate as
     * well, of course.
     */

    /*
     * Try to retrieve a CRL corresponding to the _subject_ of
     * the current certificate in order to verify it's integrity.
     */
    memset((char *)&obj, 0, sizeof(obj));
    X509_STORE_CTX_init(&store_ctx, crl_store, NULL, NULL);
    rc = X509_STORE_get_by_subject(&store_ctx, X509_LU_CRL, subject, &obj);
    X509_STORE_CTX_cleanup(&store_ctx);
    crl = obj.data.crl;
    if (rc > 0 && crl != NULL) {
        /*
         * Verify the signature on this CRL
         */
        if (X509_CRL_verify(crl, X509_get_pubkey(xs)) <= 0) {
            Log::global->Format(0,"Invalid signature on CRL!\n");
            X509_STORE_CTX_set_error(ctx, X509_V_ERR_CRL_SIGNATURE_FAILURE);
            X509_OBJECT_free_contents(&obj);
            return 0;
        }

        /*
         * Check date of CRL to make sure it's not expired
         */
        i = X509_cmp_current_time(X509_CRL_get_nextUpdate(crl));
        if (i == 0) {
            Log::global->Format(0,"Found CRL has invalid nextUpdate field.\n");
            X509_STORE_CTX_set_error(ctx, X509_V_ERR_ERROR_IN_CRL_NEXT_UPDATE_FIELD);
            X509_OBJECT_free_contents(&obj);
            return 0;
        }
        if (i < 0) {
            Log::global->Format(0,"Found CRL is expired - revoking all certificates until you get updated CRL.\n");
            X509_STORE_CTX_set_error(ctx, X509_V_ERR_CRL_HAS_EXPIRED);
            X509_OBJECT_free_contents(&obj);
            return 0;
        }
        X509_OBJECT_free_contents(&obj);
    }

    /*
     * Try to retrieve a CRL corresponding to the _issuer_ of
     * the current certificate in order to check for revocation.
     */
    memset((char *)&obj, 0, sizeof(obj));
    X509_STORE_CTX_init(&store_ctx, crl_store, NULL, NULL);
    rc = X509_STORE_get_by_subject(&store_ctx, X509_LU_CRL, issuer, &obj);
    X509_STORE_CTX_cleanup(&store_ctx);
    crl = obj.data.crl;
    if (rc > 0 && crl != NULL) {
        /*
         * Check if the current certificate is revoked by this CRL
         */
        n = sk_X509_REVOKED_num(X509_CRL_get_REVOKED(crl));
        for (i = 0; i < n; i++) {
            revoked = sk_X509_REVOKED_value(X509_CRL_get_REVOKED(crl), i);
            if (ASN1_INTEGER_cmp(revoked->serialNumber, X509_get_serialNumber(xs)) == 0) {
                serial = ASN1_INTEGER_get(revoked->serialNumber);
                cp = X509_NAME_oneline(issuer, NULL, 0);
                Log::global->Format(0,
		    "Certificate with serial %ld (0x%lX) revoked per CRL from issuer %s\n",
                        serial, serial, cp ? cp : "(ERROR)");
                free(cp);

                X509_STORE_CTX_set_error(ctx, X509_V_ERR_CERT_REVOKED);
                X509_OBJECT_free_contents(&obj);
                return 0;
            }
        }
        X509_OBJECT_free_contents(&obj);
    }
    return 1;
}

static int lftp_ssl_verify_callback(int ok,X509_STORE_CTX *ctx)
{
   static X509 *prev_cert=0;
   X509 *cert=X509_STORE_CTX_get_current_cert(ctx);

   if(cert!=prev_cert)
   {
      int depth          = X509_STORE_CTX_get_error_depth(ctx);
      X509_NAME *subject = X509_get_subject_name(cert);
      X509_NAME *issuer  = X509_get_issuer_name(cert);
      char *subject_line = X509_NAME_oneline(subject, NULL, 0);
      char *issuer_line  = X509_NAME_oneline(issuer, NULL, 0);
      Log::global->Format(3,"Certificate depth: %d; subject: %s; issuer: %s\n",
			  depth,subject_line,issuer_line);
      free(subject_line);
      free(issuer_line);
   }

   if(ok && !lftp_ssl_verify_crl(ctx))
      ok=0;

   int error=X509_STORE_CTX_get_error(ctx);

   bool verify=ResMgr::QueryBool("ssl:verify-certificate",host);

   if(!ok)
   {
      Log::global->Format(0,"%s: Certificate verification: %s\n",
			  verify?"ERROR":"WARNING",
			  X509_verify_cert_error_string(error));
   }

   if(!verify)
      ok=1;

   if(!ok)
      certificate_verify_error=error;

   prev_cert=cert;
   return ok;
}
#endif // USE_OPENSSL

#endif // USE_SSL
