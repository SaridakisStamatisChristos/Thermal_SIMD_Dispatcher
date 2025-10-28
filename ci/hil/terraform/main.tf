terraform {
  required_version = ">= 1.4.0"
  required_providers {
    aws = {
      source  = "hashicorp/aws"
      version = "~> 5.0"
    }
  }
}

provider "aws" {
  region = var.region
}

resource "aws_security_group" "hil_runner" {
  name        = "${var.project}-hil-runner"
  description = "Allow CI access to hardware-in-the-loop runner"
  vpc_id      = var.vpc_id

  ingress {
    description = "Allow SSH from CI orchestrator"
    from_port   = 22
    to_port     = 22
    protocol    = "tcp"
    cidr_blocks = var.allowed_ssh_cidrs
  }

  egress {
    from_port   = 0
    to_port     = 0
    protocol    = "-1"
    cidr_blocks = ["0.0.0.0/0"]
  }
}

resource "aws_iam_instance_profile" "hil_runner" {
  name = "${var.project}-hil-runner"
  role = aws_iam_role.hil_runner.name
}

resource "aws_iam_role" "hil_runner" {
  name = "${var.project}-hil-runner"

  assume_role_policy = jsonencode({
    Version = "2012-10-17"
    Statement = [
      {
        Action = "sts:AssumeRole"
        Principal = {
          Service = "ec2.amazonaws.com"
        }
        Effect = "Allow"
        Sid    = ""
      }
    ]
  })
}

resource "aws_iam_role_policy" "hil_runner" {
  name = "${var.project}-hil-runner"
  role = aws_iam_role.hil_runner.id

  policy = jsonencode({
    Version = "2012-10-17"
    Statement = [
      {
        Action = [
          "ec2:DescribeInstances",
          "ec2:DescribeInstanceAttribute"
        ]
        Effect   = "Allow"
        Resource = "*"
      }
    ]
  })
}

locals {
  runner_tags = merge(var.tags, {
    Project     = var.project,
    Environment = var.environment,
    Purpose     = "thermal-simd-hil"
  })
}

resource "aws_instance" "hil_runner" {
  ami                         = var.ami_id
  instance_type               = var.instance_type
  subnet_id                   = var.subnet_id
  associate_public_ip_address = var.associate_public_ip
  iam_instance_profile        = aws_iam_instance_profile.hil_runner.name
  key_name                    = var.ssh_key_name
  vpc_security_group_ids      = [aws_security_group.hil_runner.id]

  cpu_options {
    core_count       = var.core_count
    threads_per_core = 2
  }

  metadata_options {
    http_endpoint = "enabled"
  }

  user_data = templatefile("${path.module}/user_data.sh.tpl", {
    runner_registration_token = var.runner_registration_token
    runner_tags               = join(",", var.runner_tags)
    runner_name               = "${var.project}-hil-runner"
    runner_coordinator_url    = var.runner_coordinator_url
  })

  root_block_device {
    volume_size = var.root_volume_size
    volume_type = "gp3"
    encrypted   = true
  }

  tags = local.runner_tags
}

output "runner_public_ip" {
  description = "Public IPv4 address for SSH access"
  value       = aws_instance.hil_runner.public_ip
}

output "runner_private_ip" {
  description = "Private IPv4 address for VPC internal access"
  value       = aws_instance.hil_runner.private_ip
}
