/*
 * Copyright (C) 2025 The pgexporter community
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this list
 * of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice, this
 * list of conditions and the following disclaimer in the documentation and/or other
 * materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors may
 * be used to endorse or promote products derived from this software without specific
 * prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
 * TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <pgexporter.h>
#include <query_alts_ext.h>
#include <shmem.h>
#include <utils.h>

// Get height of extension AVL Tree Node
static int ext_height(struct ext_query_alts* A);

// Get balance of extension AVL Tree Node
static int ext_get_node_balance(struct ext_query_alts* A);

// Right rotate extension node
static struct ext_query_alts* ext_node_right_rotate(struct ext_query_alts* root);

// Left rotate extension node
static struct ext_query_alts* ext_node_left_rotate(struct ext_query_alts* root);

void
pgexporter_copy_extension_query_alts(struct ext_query_alts** dst, struct ext_query_alts* src)
{
   if (!src)
   {
      return;
   }

   void* new_query_alt = NULL;

   pgexporter_create_shared_memory(sizeof(struct ext_query_alts), HUGEPAGE_OFF, &new_query_alt);
   *dst = (struct ext_query_alts*) new_query_alt;

   (*dst)->height = src->height;
   (*dst)->ext_version = src->ext_version;
   (*dst)->node.is_histogram = src->node.is_histogram;
   (*dst)->node.n_columns = src->node.n_columns;

   memcpy((*dst)->node.query, src->node.query, MAX_QUERY_LENGTH);
   memcpy((*dst)->node.columns, src->node.columns, MAX_NUMBER_OF_COLUMNS * sizeof(struct column));

   pgexporter_copy_extension_query_alts(&(*dst)->left, src->left);
   pgexporter_copy_extension_query_alts(&(*dst)->right, src->right);
}

static int
ext_height(struct ext_query_alts* A)
{
   return A ? A->height : 0;
}

static int
ext_get_node_balance(struct ext_query_alts* A)
{
   return A ? ext_height(A->left) - ext_height(A->right) : 0;
}

static struct ext_query_alts*
ext_node_right_rotate(struct ext_query_alts* root)
{
   if (!root || !root->left)
   {
      return root;
   }

   struct ext_query_alts* A = root;
   struct ext_query_alts* B = root->left;

   A->left = B->right;
   B->right = A;

   A->height = MAX(ext_height(A->left), ext_height(A->right)) + 1;
   B->height = MAX(ext_height(B->left), ext_height(B->right)) + 1;

   return B;
}

static struct ext_query_alts*
ext_node_left_rotate(struct ext_query_alts* root)
{
   if (!root || !root->right)
   {
      return root;
   }

   struct ext_query_alts* A = root;
   struct ext_query_alts* B = root->right;

   A->right = B->left;
   B->left = A;

   A->height = MAX(ext_height(A->left), ext_height(A->right)) + 1;
   B->height = MAX(ext_height(B->left), ext_height(B->right)) + 1;

   return B;
}

struct ext_query_alts*
pgexporter_insert_extension_node_avl(struct ext_query_alts* root, struct ext_query_alts** new_node)
{
   if (!root)
   {
      return (*new_node);
   }

   int cmp = pgexporter_compare_extension_versions(&root->ext_version, &(*new_node)->ext_version);

   if (cmp == VERSION_EQUAL)
   {
      // Free new node, as no need to insert it
      pgexporter_free_extension_node_avl(new_node);
      return root;
   }
   else if (cmp == VERSION_GREATER)
   {
      root->left = pgexporter_insert_extension_node_avl(root->left, new_node);
   }
   else
   {
      root->right = pgexporter_insert_extension_node_avl(root->right, new_node);
   }

   root->height = MAX(ext_height(root->left), ext_height(root->right)) + 1;

   /* AVL Rotations */
   if (ext_get_node_balance(root) > 1)
   {
      if (ext_get_node_balance(root->left) == -1)
      {
         root->left = ext_node_left_rotate(root->left);
      }
      return ext_node_right_rotate(root);
   }
   else if (ext_get_node_balance(root) < -1)
   {
      if (ext_get_node_balance(root->right) == 1)
      {
         root->right = ext_node_right_rotate(root->right);
      }
      if (ext_get_node_balance(root) != 0)
      {
         return ext_node_left_rotate(root);
      }
   }

   return root;
}

struct ext_query_alts*
pgexporter_get_extension_query_alt(struct ext_query_alts* root, struct version* ext_version)
{
   struct ext_query_alts* temp = root;
   struct ext_query_alts* last = NULL;

   // Traversing the AVL tree to find highest compatible version
   while (temp)
   {
      int cmp = pgexporter_compare_extension_versions(&temp->ext_version, ext_version);

      if (cmp <= VERSION_EQUAL &&
          (!last || pgexporter_compare_extension_versions(&temp->ext_version, &last->ext_version) == VERSION_GREATER))
      {
         last = temp;
      }

      if (cmp == VERSION_GREATER)
      {
         temp = temp->left;
      }
      else
      {
         temp = temp->right;
      }
   }

   if (!last || pgexporter_compare_extension_versions(&last->ext_version, ext_version) == VERSION_GREATER)
   {
      return NULL;
   }
   else
   {
      return last;
   }
}

void
pgexporter_free_extension_query_alts(struct prometheus* prom)
{
   if (prom && prom->ext_root)
   {
      pgexporter_free_extension_node_avl(&prom->ext_root);
   }
}

void
pgexporter_free_extension_node_avl(struct ext_query_alts** root)
{
   if (!root || !(*root))
   {
      return;
   }

   pgexporter_free_extension_node_avl(&(*root)->left);
   pgexporter_free_extension_node_avl(&(*root)->right);

   pgexporter_destroy_shared_memory(&root, sizeof(struct ext_query_alts*));
}
