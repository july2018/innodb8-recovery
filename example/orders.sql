-- Example: Recover data from `orders` table
-- Save this as: orders.sql

CREATE TABLE `orders` (
  `id`          INT          NOT NULL AUTO_INCREMENT,
  `user_id`     INT          NOT NULL,
  `product`     VARCHAR(255) NOT NULL,
  `quantity`    SMALLINT     NOT NULL DEFAULT 1,
  `price`       DECIMAL(10,2) NOT NULL,
  `status`      TINYINT      NOT NULL DEFAULT 0,
  `note`        TEXT,
  `created_at`  DATETIME     NOT NULL,
  `updated_at`  TIMESTAMP,
  PRIMARY KEY (`id`),
  KEY `idx_user` (`user_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
